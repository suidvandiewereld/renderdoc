/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// Implements 'renderdoccmd mcp' - a Model Context Protocol server speaking JSON-RPC 2.0 over stdio,
// exposing capture loading, analysis and programmatic capturing as MCP tools.

#include <ctype.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <replay/version.h>
#include "mcp/mcp_json.h"
#include "renderdoccmd.h"

// the PipeState helpers are implemented inline for API consumers, and must be compiled exactly once
#include <replay/pipestate.inl>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)    // function unsafe (fopen vs fopen_s)
#pragma warning(disable : 4456)    // declaration hides previous local declaration
#pragma warning(disable : 4457)    // declaration hides function parameter
#pragma warning(disable : 4189)    // local variable is initialized but not referenced
#pragma warning(disable : 4244)    // narrowing conversion, possible loss of data
#pragma warning(disable : 4702)    // unreachable code
#pragma warning(disable : 4505)    // unreferenced local function has been removed
#pragma warning(disable : 4245)    // signed/unsigned mismatch
#pragma warning(disable : 4100)    // unreferenced formal parameter
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_ASSERT(x)
#include "../renderdoc/3rdparty/stb/stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x)
#include "../renderdoc/3rdparty/stb/stb_image_write.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

rdcstr conv(const std::string &s);
std::string conv(const rdcstr &s);

using mcp::Json;

namespace
{
static const char *const SupportedProtocolVersions[] = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};

// cap the size of any single text result so a careless query can't flood the client's context
static const size_t MaxTextResultBytes = 512 * 1024;

struct ToolError : public std::runtime_error
{
  explicit ToolError(const std::string &msg) : std::runtime_error(msg) {}
};

std::string str(const rdcstr &s)
{
  return std::string(s.c_str(), s.size());
}

std::string str(const rdcinflexiblestr &s)
{
  return std::string(s.c_str());
}

std::string lower(std::string s)
{
  for(char &c : s)
    c = (char)tolower((unsigned char)c);
  return s;
}

bool containsNoCase(const std::string &haystack, const std::string &needle)
{
  if(needle.empty())
    return true;
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

uint64_t IdNum(ResourceId id)
{
  static_assert(sizeof(ResourceId) == sizeof(uint64_t), "ResourceId is expected to be 64-bit");
  uint64_t ret = 0;
  memcpy(&ret, (const void *)&id, sizeof(ret));
  return ret;
}

ResourceId IdFromNum(uint64_t num)
{
  ResourceId ret;
  memcpy((void *)&ret, &num, sizeof(num));
  return ret;
}

std::string Base64(const std::vector<unsigned char> &data)
{
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string ret;
  ret.reserve(((data.size() + 2) / 3) * 4);
  size_t i = 0;
  for(; i + 2 < data.size(); i += 3)
  {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
    ret.push_back(table[(v >> 18) & 63]);
    ret.push_back(table[(v >> 12) & 63]);
    ret.push_back(table[(v >> 6) & 63]);
    ret.push_back(table[v & 63]);
  }
  if(i < data.size())
  {
    uint32_t v = uint32_t(data[i]) << 16;
    if(i + 1 < data.size())
      v |= uint32_t(data[i + 1]) << 8;
    ret.push_back(table[(v >> 18) & 63]);
    ret.push_back(table[(v >> 12) & 63]);
    ret.push_back(i + 1 < data.size() ? table[(v >> 6) & 63] : '=');
    ret.push_back('=');
  }
  return ret;
}

std::string TempFilePath(const char *ext)
{
  static uint32_t counter = 0;
  const char *dir = getenv("TMPDIR");
  if(!dir || !dir[0])
    dir = getenv("TEMP");
  if(!dir || !dir[0])
    dir = getenv("TMP");
#if defined(_WIN32)
  if(!dir || !dir[0])
    dir = ".";
  const char sep = '\\';
#else
  if(!dir || !dir[0])
    dir = "/tmp";
  const char sep = '/';
#endif
  return std::string(dir) + sep + "renderdoc_mcp_" + std::to_string((uint64_t)time(NULL)) + "_" +
         std::to_string(counter++) + ext;
}

bool ReadWholeFile(const std::string &path, std::vector<unsigned char> &out)
{
  FILE *f = fopen(path.c_str(), "rb");
  if(!f)
    return false;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(len < 0)
  {
    fclose(f);
    return false;
  }
  out.resize((size_t)len);
  size_t read = len > 0 ? fread(out.data(), 1, (size_t)len, f) : 0;
  fclose(f);
  return read == (size_t)len;
}

void PngWriteCallback(void *context, void *data, int size)
{
  std::vector<unsigned char> *out = (std::vector<unsigned char> *)context;
  const unsigned char *bytes = (const unsigned char *)data;
  out->insert(out->end(), bytes, bytes + size);
}

// Box-filter a PNG down so that neither dimension exceeds maxDim. Returns false if the PNG couldn't
// be decoded, in which case the original data should be used as-is.
bool DownscalePNG(std::vector<unsigned char> &png, int maxDim, int &outW, int &outH)
{
  int w = 0, h = 0, comp = 0;
  unsigned char *pixels = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &comp, 4);
  if(!pixels)
    return false;

  outW = w;
  outH = h;

  if(w <= maxDim && h <= maxDim)
  {
    stbi_image_free(pixels);
    return true;
  }

  double scale = double(maxDim) / double(std::max(w, h));
  int dw = std::max(1, int(w * scale));
  int dh = std::max(1, int(h * scale));

  std::vector<unsigned char> dst(size_t(dw) * dh * 4);
  for(int y = 0; y < dh; y++)
  {
    int sy0 = int(int64_t(y) * h / dh);
    int sy1 = std::max(sy0 + 1, int(int64_t(y + 1) * h / dh));
    for(int x = 0; x < dw; x++)
    {
      int sx0 = int(int64_t(x) * w / dw);
      int sx1 = std::max(sx0 + 1, int(int64_t(x + 1) * w / dw));
      uint32_t sum[4] = {0, 0, 0, 0};
      uint32_t count = 0;
      for(int sy = sy0; sy < sy1 && sy < h; sy++)
      {
        for(int sx = sx0; sx < sx1 && sx < w; sx++)
        {
          const unsigned char *p = pixels + (size_t(sy) * w + sx) * 4;
          for(int c = 0; c < 4; c++)
            sum[c] += p[c];
          count++;
        }
      }
      unsigned char *d = dst.data() + (size_t(y) * dw + x) * 4;
      for(int c = 0; c < 4; c++)
        d[c] = (unsigned char)(count ? sum[c] / count : 0);
    }
  }
  stbi_image_free(pixels);

  std::vector<unsigned char> encoded;
  if(!stbi_write_png_to_func(PngWriteCallback, &encoded, dw, dh, 4, dst.data(), dw * 4))
    return false;

  png.swap(encoded);
  outW = dw;
  outH = dh;
  return true;
}

///////////////////////////////////////////////////////////////////////////////
// argument helpers

const Json &Arg(const Json &args, const char *name)
{
  return args[name];
}

bool HasArg(const Json &args, const char *name)
{
  const Json *v = args.find(name);
  return v && !v->isNull();
}

std::string ArgString(const Json &args, const char *name, const std::string &def = std::string())
{
  const Json &v = Arg(args, name);
  if(v.isString())
    return v.asString();
  if(v.isNull())
    return def;
  return v.dump();
}

std::string RequireString(const Json &args, const char *name)
{
  const Json &v = Arg(args, name);
  if(!v.isString() || v.asString().empty())
    throw ToolError(std::string("Missing required string argument '") + name + "'");
  return v.asString();
}

uint64_t ArgUInt(const Json &args, const char *name, uint64_t def)
{
  const Json &v = Arg(args, name);
  if(v.isNumber())
    return v.asUInt(def);
  if(v.isString() && !v.asString().empty())
    return strtoull(v.asString().c_str(), NULL, 0);
  return def;
}

int64_t ArgInt(const Json &args, const char *name, int64_t def)
{
  const Json &v = Arg(args, name);
  if(v.isNumber())
    return v.asInt(def);
  if(v.isString() && !v.asString().empty())
    return strtoll(v.asString().c_str(), NULL, 0);
  return def;
}

double ArgDouble(const Json &args, const char *name, double def)
{
  const Json &v = Arg(args, name);
  if(v.isNumber())
    return v.asDouble(def);
  if(v.isString() && !v.asString().empty())
    return strtod(v.asString().c_str(), NULL);
  return def;
}

bool ArgBool(const Json &args, const char *name, bool def)
{
  const Json &v = Arg(args, name);
  if(v.isBool())
    return v.asBool();
  if(v.isNumber())
    return v.asInt() != 0;
  if(v.isString())
  {
    std::string s = lower(v.asString());
    if(s == "true" || s == "1" || s == "yes")
      return true;
    if(s == "false" || s == "0" || s == "no")
      return false;
  }
  return def;
}

// accepts 123, "123", "ResourceId::123"
bool ParseResourceId(const Json &v, ResourceId &out)
{
  if(v.isNumber())
  {
    out = IdFromNum(v.asUInt());
    return true;
  }
  if(v.isString())
  {
    std::string s = v.asString();
    size_t colon = s.rfind(':');
    if(colon != std::string::npos)
      s = s.substr(colon + 1);
    while(!s.empty() && s[0] == ' ')
      s.erase(s.begin());
    if(s.empty() || !isdigit((unsigned char)s[0]))
      return false;
    out = IdFromNum(strtoull(s.c_str(), NULL, 10));
    return true;
  }
  return false;
}

ShaderStage ParseStage(const std::string &name)
{
  std::string s = lower(name);
  if(s == "vertex" || s == "vs" || s == "vert")
    return ShaderStage::Vertex;
  if(s == "hull" || s == "hs" || s == "tess_control" || s == "tesscontrol" || s == "tcs")
    return ShaderStage::Hull;
  if(s == "domain" || s == "ds" || s == "tess_eval" || s == "tesseval" || s == "tes")
    return ShaderStage::Domain;
  if(s == "geometry" || s == "gs" || s == "geom")
    return ShaderStage::Geometry;
  if(s == "pixel" || s == "ps" || s == "fragment" || s == "fs" || s == "frag")
    return ShaderStage::Pixel;
  if(s == "compute" || s == "cs" || s == "comp")
    return ShaderStage::Compute;
  if(s == "task" || s == "amplification" || s == "as" || s == "ts")
    return ShaderStage::Task;
  if(s == "mesh" || s == "ms")
    return ShaderStage::Mesh;
  throw ToolError("Unknown shader stage '" + name +
                  "'. Use one of: vertex, hull, domain, geometry, pixel, compute, task, mesh");
}

const ShaderStage GraphicsStages[] = {
    ShaderStage::Vertex, ShaderStage::Hull, ShaderStage::Domain, ShaderStage::Geometry,
    ShaderStage::Pixel,  ShaderStage::Task, ShaderStage::Mesh,
};

///////////////////////////////////////////////////////////////////////////////
// JSON schema helpers for tool definitions

struct Prop
{
  const char *name;
  const char *type;
  const char *description;
  bool required;
};

Json MakeSchema(std::initializer_list<Prop> props)
{
  Json schema = Json::object();
  schema["type"] = "object";
  Json &properties = schema["properties"];
  properties = Json::object();
  Json required = Json::array();
  for(const Prop &p : props)
  {
    Json &prop = properties[p.name];
    prop["type"] = p.type;
    prop["description"] = p.description;
    if(p.required)
      required.push_back(p.name);
  }
  if(required.size() > 0)
    schema["required"] = required;
  return schema;
}

///////////////////////////////////////////////////////////////////////////////
// the server

class McpServer
{
public:
  explicit McpServer(FILE *out) : m_Out(out) { RegisterTools(); }
  ~McpServer() { CloseCapture(); }

  int Run(std::istream &in);

  void OpenCaptureInternal(const std::string &path);

private:
  struct ToolResult
  {
    Json content = Json::array();
    bool isError = false;
  };

  typedef std::function<ToolResult(const Json &)> ToolHandler;

  struct Tool
  {
    std::string name;
    std::string title;
    std::string description;
    Json inputSchema;
    bool readOnly;
    ToolHandler handler;
  };

  void AddTool(const char *name, const char *title, const char *description, Json schema,
               bool readOnly, ToolHandler handler)
  {
    Tool t;
    t.name = name;
    t.title = title;
    t.description = description;
    t.inputSchema = std::move(schema);
    t.readOnly = readOnly;
    t.handler = std::move(handler);
    m_Tools.push_back(std::move(t));
  }

  void RegisterTools();

  void Send(const Json &msg);
  Json HandleMessage(const Json &msg, bool &hasResponse);
  Json HandleToolCall(const Json &params, Json &error);

  static Json ErrorObject(int code, const std::string &message)
  {
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    return err;
  }

  static ToolResult TextResult(const Json &data)
  {
    ToolResult ret;
    Json text = Json::object();
    text["type"] = "text";
    std::string s = data.isString() ? data.asString() : data.dump();
    if(s.size() > MaxTextResultBytes)
    {
      s.resize(MaxTextResultBytes);
      s += "\n... [output truncated - narrow the query with offset/limit/filter arguments]";
    }
    text["text"] = std::move(s);
    ret.content.push_back(std::move(text));
    return ret;
  }

  // capture state
  void CloseCapture();
  void RequireCapture() const
  {
    if(!m_Ctrl)
      throw ToolError("No capture is open. Call open_capture (or capture_program) first.");
  }
  void BuildIndices();
  void IndexAction(const ActionDescription &action, const ActionDescription *parent);
  void PullDebugMessages();
  uint32_t ResolveEvent(const Json &args);
  void SetEvent(uint32_t eventId);
  const ActionDescription *ActionForEvent(uint32_t eventId) const;
  ResourceId ResolveTexture(const Json &args, uint32_t eventId);
  const TextureDescription *FindTexture(ResourceId id) const;
  const BufferDescription *FindBuffer(ResourceId id) const;
  std::string ResName(ResourceId id) const;
  Json ResRef(ResourceId id) const;
  std::string ActionName(const ActionDescription &action) const;
  Json CaptureSummary() const;
  Json ActionRow(const ActionDescription &action, int depth) const;
  Json SDToJson(const SDObject *obj, int depth, uint32_t maxArray) const;
  Json DescriptorJson(const Descriptor &desc, bool includeType = true) const;
  Json UsedDescriptorJson(const UsedDescriptor &used, const ShaderReflection *refl) const;
  Json ShaderVariableJson(const ShaderVariable &var) const;
  Json SubresourceArgs(const Json &args, Subresource &sub) const;

  // tool implementations
  ToolResult ToolOpenCapture(const Json &args);
  ToolResult ToolCloseCapture(const Json &args);
  ToolResult ToolCaptureInfo(const Json &args);
  ToolResult ToolListActions(const Json &args);
  ToolResult ToolGetAction(const Json &args);
  ToolResult ToolGetApiCalls(const Json &args);
  ToolResult ToolPipelineState(const Json &args);
  ToolResult ToolGetShader(const Json &args);
  ToolResult ToolGetCBuffer(const Json &args);
  ToolResult ToolListResources(const Json &args);
  ToolResult ToolGetResource(const Json &args);
  ToolResult ToolViewTexture(const Json &args);
  ToolResult ToolSaveTexture(const Json &args);
  ToolResult ToolPickPixel(const Json &args);
  ToolResult ToolPixelHistory(const Json &args);
  ToolResult ToolBufferData(const Json &args);
  ToolResult ToolDebugMessages(const Json &args);
  ToolResult ToolCaptureProgram(const Json &args);

  FILE *m_Out;
  bool m_Initialized = false;
  std::vector<Tool> m_Tools;

  IReplayController *m_Ctrl = NULL;
  std::string m_CapturePath;
  APIProperties m_Props;
  uint32_t m_CurEvent = 0;
  uint32_t m_LastEvent = 0;
  std::vector<const ActionDescription *> m_Linear;
  std::map<uint32_t, size_t> m_ActionIndex;    // action eventId -> index in m_Linear
  std::map<uint32_t, const ActionDescription *> m_EventToAction;
  std::map<const ActionDescription *, const ActionDescription *> m_Parent;
  std::map<uint64_t, std::string> m_Names;
  std::map<uint64_t, size_t> m_TexIndex;
  std::map<uint64_t, size_t> m_BufIndex;
  std::vector<DebugMessage> m_Messages;
};

void McpServer::Send(const Json &msg)
{
  std::string line = msg.dump();
  line.push_back('\n');
  fwrite(line.data(), 1, line.size(), m_Out);
  fflush(m_Out);
}

int McpServer::Run(std::istream &in)
{
  std::string line;
  while(std::getline(in, line))
  {
    while(!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if(line.empty())
      continue;

    Json msg;
    std::string parseError;
    if(!Json::parse(line, msg, parseError))
    {
      Json resp = Json::object();
      resp["jsonrpc"] = "2.0";
      resp["id"] = Json();
      resp["error"] = ErrorObject(-32700, "Parse error: " + parseError);
      Send(resp);
      continue;
    }

    if(msg.isArray())
    {
      // JSON-RPC batch
      Json responses = Json::array();
      for(const Json &m : msg.items())
      {
        bool hasResponse = false;
        Json resp = HandleMessage(m, hasResponse);
        if(hasResponse)
          responses.push_back(std::move(resp));
      }
      if(responses.size() > 0)
        Send(responses);
      continue;
    }

    bool hasResponse = false;
    Json resp = HandleMessage(msg, hasResponse);
    if(hasResponse)
      Send(resp);
  }

  return 0;
}

Json McpServer::HandleMessage(const Json &msg, bool &hasResponse)
{
  Json resp = Json::object();
  resp["jsonrpc"] = "2.0";

  if(!msg.isObject())
  {
    hasResponse = true;
    resp["id"] = Json();
    resp["error"] = ErrorObject(-32600, "Invalid request");
    return resp;
  }

  const Json *id = msg.find("id");
  const std::string &method = msg["method"].asString();

  // responses to requests we never sent, or notifications, get no reply
  hasResponse = (id != NULL) && !method.empty();
  if(id)
    resp["id"] = *id;

  if(method.empty())
  {
    if(id && !msg.has("result") && !msg.has("error"))
    {
      hasResponse = true;
      resp["error"] = ErrorObject(-32600, "Invalid request: missing method");
    }
    return resp;
  }

  const Json &params = msg["params"];

  if(method == "initialize")
  {
    std::string requested = params["protocolVersion"].asString();
    std::string version = SupportedProtocolVersions[0];
    for(const char *v : SupportedProtocolVersions)
      if(requested == v)
        version = v;

    Json result = Json::object();
    result["protocolVersion"] = version;
    Json &caps = result["capabilities"];
    caps = Json::object();
    caps["tools"]["listChanged"] = false;
    Json &info = result["serverInfo"];
    info["name"] = "renderdoc";
    info["title"] = "RenderDoc";
    info["version"] = FULL_VERSION_STRING;
    result["instructions"] =
        "RenderDoc graphics debugger. Typical workflow: open_capture (or capture_program to launch "
        "and capture an application), then list_actions to browse the frame's draws/dispatches, "
        "get_pipeline_state / get_shader / get_cbuffer_contents at an event to inspect GPU state, "
        "view_texture to look at render targets or textures, and pick_pixel / pixel_history to "
        "debug specific pixels. Event IDs (eventId) identify points in the frame; resource IDs "
        "are integers. Most tools default to the current event (the last one set) if event_id is "
        "omitted.";
    resp["result"] = result;
    return resp;
  }

  if(method == "notifications/initialized")
  {
    m_Initialized = true;
    return resp;
  }

  if(method == "ping")
  {
    resp["result"] = Json::object();
    return resp;
  }

  if(method == "tools/list")
  {
    Json result = Json::object();
    Json &tools = result["tools"];
    tools = Json::array();
    for(const Tool &t : m_Tools)
    {
      Json tool = Json::object();
      tool["name"] = t.name;
      tool["title"] = t.title;
      tool["description"] = t.description;
      tool["inputSchema"] = t.inputSchema;
      Json &annotations = tool["annotations"];
      annotations["title"] = t.title;
      annotations["readOnlyHint"] = t.readOnly;
      annotations["openWorldHint"] = false;
      tools.push_back(std::move(tool));
    }
    resp["result"] = result;
    return resp;
  }

  if(method == "tools/call")
  {
    Json error;
    Json result = HandleToolCall(params, error);
    if(!error.isNull())
      resp["error"] = error;
    else
      resp["result"] = result;
    return resp;
  }

  if(method.compare(0, 14, "notifications/") == 0)
    return resp;

  resp["error"] = ErrorObject(-32601, "Method not found: " + method);
  return resp;
}

Json McpServer::HandleToolCall(const Json &params, Json &error)
{
  const std::string &name = params["name"].asString();
  const Json &args = params["arguments"].isObject() ? params["arguments"] : Json::object();

  const Tool *tool = NULL;
  for(const Tool &t : m_Tools)
    if(t.name == name)
      tool = &t;

  if(!tool)
  {
    error = ErrorObject(-32602, "Unknown tool: " + name);
    return Json();
  }

  ToolResult res;
  try
  {
    res = tool->handler(args);
  }
  catch(ToolError &e)
  {
    res = TextResult(Json(std::string(e.what())));
    res.isError = true;
  }
  catch(std::exception &e)
  {
    res = TextResult(Json(std::string("Internal error: ") + e.what()));
    res.isError = true;
  }

  Json result = Json::object();
  result["content"] = res.content;
  if(res.isError)
    result["isError"] = true;
  return result;
}

///////////////////////////////////////////////////////////////////////////////
// capture state management

void McpServer::CloseCapture()
{
  if(m_Ctrl)
    m_Ctrl->Shutdown();
  m_Ctrl = NULL;
  m_CapturePath.clear();
  m_CurEvent = m_LastEvent = 0;
  m_Linear.clear();
  m_ActionIndex.clear();
  m_EventToAction.clear();
  m_Parent.clear();
  m_Names.clear();
  m_TexIndex.clear();
  m_BufIndex.clear();
  m_Messages.clear();
}

void McpServer::OpenCaptureInternal(const std::string &path)
{
  CloseCapture();

  ICaptureFile *file = RENDERDOC_OpenCaptureFile();
  ResultDetails res = file->OpenFile(conv(path), "rdc", NULL);
  if(res.code != ResultCode::Succeeded)
  {
    file->Shutdown();
    throw ToolError("Couldn't open '" + path + "': " + str(res.Message()));
  }

  ReplaySupport support = file->LocalReplaySupport();
  if(support == ReplaySupport::Unsupported)
  {
    std::string driver = str(file->DriverName());
    file->Shutdown();
    throw ToolError("Capture '" + path + "' (" + driver +
                    ") can't be replayed on this machine. It may need a different GPU/driver or a "
                    "remote replay context.");
  }

  IReplayController *ctrl = NULL;
  rdctie(res, ctrl) = file->OpenCapture(ReplayOptions(), NULL);
  file->Shutdown();

  if(!res.OK() || ctrl == NULL)
    throw ToolError("Couldn't replay '" + path + "': " + str(res.Message()));

  m_Ctrl = ctrl;
  m_CapturePath = path;
  m_Props = m_Ctrl->GetAPIProperties();

  BuildIndices();

  FrameDescription frame = m_Ctrl->GetFrameInfo();
  for(const DebugMessage &msg : frame.debugMessages)
    m_Messages.push_back(msg);
  PullDebugMessages();

  if(m_LastEvent != 0)
    SetEvent(m_LastEvent);
}

void McpServer::IndexAction(const ActionDescription &action, const ActionDescription *parent)
{
  m_ActionIndex[action.eventId] = m_Linear.size();
  m_Linear.push_back(&action);
  m_Parent[&action] = parent;
  for(const APIEvent &ev : action.events)
    m_EventToAction[ev.eventId] = &action;
  m_EventToAction[action.eventId] = &action;
  m_LastEvent = std::max(m_LastEvent, action.eventId);

  for(const ActionDescription &child : action.children)
    IndexAction(child, &action);
}

void McpServer::BuildIndices()
{
  for(const ActionDescription &action : m_Ctrl->GetRootActions())
    IndexAction(action, NULL);

  for(const ResourceDescription &res : m_Ctrl->GetResources())
    m_Names[IdNum(res.resourceId)] = str(res.name);

  const rdcarray<TextureDescription> &texs = m_Ctrl->GetTextures();
  for(size_t i = 0; i < texs.size(); i++)
    m_TexIndex[IdNum(texs[i].resourceId)] = i;

  const rdcarray<BufferDescription> &bufs = m_Ctrl->GetBuffers();
  for(size_t i = 0; i < bufs.size(); i++)
    m_BufIndex[IdNum(bufs[i].resourceId)] = i;
}

void McpServer::PullDebugMessages()
{
  if(!m_Ctrl)
    return;
  for(const DebugMessage &msg : m_Ctrl->GetDebugMessages())
    m_Messages.push_back(msg);
}

const ActionDescription *McpServer::ActionForEvent(uint32_t eventId) const
{
  auto it = m_EventToAction.find(eventId);
  if(it != m_EventToAction.end())
    return it->second;
  return NULL;
}

uint32_t McpServer::ResolveEvent(const Json &args)
{
  RequireCapture();
  if(!HasArg(args, "event_id"))
    return m_CurEvent;

  uint32_t eventId = (uint32_t)ArgUInt(args, "event_id", m_CurEvent);
  if(!ActionForEvent(eventId))
    throw ToolError("Event " + std::to_string(eventId) +
                    " doesn't exist in this capture. Valid event IDs range up to " +
                    std::to_string(m_LastEvent) + "; use list_actions to find them.");
  return eventId;
}

void McpServer::SetEvent(uint32_t eventId)
{
  RequireCapture();
  if(eventId == m_CurEvent)
    return;
  m_Ctrl->SetFrameEvent(eventId, true);
  m_CurEvent = eventId;
}

const TextureDescription *McpServer::FindTexture(ResourceId id) const
{
  auto it = m_TexIndex.find(IdNum(id));
  if(it == m_TexIndex.end())
    return NULL;
  return &m_Ctrl->GetTextures()[it->second];
}

const BufferDescription *McpServer::FindBuffer(ResourceId id) const
{
  auto it = m_BufIndex.find(IdNum(id));
  if(it == m_BufIndex.end())
    return NULL;
  return &m_Ctrl->GetBuffers()[it->second];
}

std::string McpServer::ResName(ResourceId id) const
{
  auto it = m_Names.find(IdNum(id));
  if(it != m_Names.end())
    return it->second;
  return std::string();
}

Json McpServer::ResRef(ResourceId id) const
{
  if(id == ResourceId())
    return Json();
  Json ret = Json::object();
  ret["id"] = IdNum(id);
  std::string name = ResName(id);
  if(!name.empty())
    ret["name"] = name;
  return ret;
}

std::string McpServer::ActionName(const ActionDescription &action) const
{
  return str(action.GetName(m_Ctrl->GetStructuredFile()));
}

// determines which texture a texture tool should operate on: an explicit resource_id, or the first
// colour output of the action at the event (falling back to depth, then the backbuffer)
ResourceId McpServer::ResolveTexture(const Json &args, uint32_t eventId)
{
  if(HasArg(args, "resource_id"))
  {
    ResourceId id;
    if(!ParseResourceId(Arg(args, "resource_id"), id))
      throw ToolError("Invalid resource_id - pass the integer resource ID.");
    if(!FindTexture(id))
      throw ToolError("Resource " + std::to_string(IdNum(id)) +
                      " is not a texture. Use list_resources with type='Texture'.");
    return id;
  }

  const ActionDescription *action = ActionForEvent(eventId);
  if(action)
  {
    for(ResourceId out : action->outputs)
      if(out != ResourceId())
        return out;
    if(action->depthOut != ResourceId())
      return action->depthOut;
    if(action->copyDestination != ResourceId() && FindTexture(action->copyDestination))
      return action->copyDestination;
  }

  for(const TextureDescription &tex : m_Ctrl->GetTextures())
    if(tex.creationFlags & TextureCategory::SwapBuffer)
      return tex.resourceId;

  throw ToolError("No resource_id given and the action at event " + std::to_string(eventId) +
                  " has no bound outputs. Pass resource_id explicitly.");
}

Json McpServer::SubresourceArgs(const Json &args, Subresource &sub) const
{
  sub.mip = (uint32_t)ArgUInt(args, "mip", 0);
  sub.slice = (uint32_t)ArgUInt(args, "slice", 0);
  sub.sample = (uint32_t)ArgUInt(args, "sample", 0);
  Json ret = Json::object();
  ret["mip"] = sub.mip;
  ret["slice"] = sub.slice;
  ret["sample"] = sub.sample;
  return ret;
}

///////////////////////////////////////////////////////////////////////////////
// JSON conversion of replay types

Json McpServer::CaptureSummary() const
{
  Json ret = Json::object();
  ret["path"] = m_CapturePath;
  ret["api"] = str(ToStr(m_Props.pipelineType));
  if(m_Props.localRenderer != m_Props.pipelineType)
    ret["replayedWith"] = str(ToStr(m_Props.localRenderer));
  ret["vendor"] = str(ToStr(m_Props.vendor));
  if(m_Props.degraded)
    ret["degraded"] = true;

  FrameDescription frame = m_Ctrl->GetFrameInfo();
  if(frame.frameNumber != FrameDescription::NoFrameNumber)
    ret["frameNumber"] = frame.frameNumber;
  ret["compressedFileSize"] = frame.compressedFileSize;

  uint32_t draws = 0, dispatches = 0, clears = 0, copies = 0, markers = 0;
  for(const ActionDescription *a : m_Linear)
  {
    if(a->flags & ActionFlags::Drawcall)
      draws++;
    if(a->flags & ActionFlags::Dispatch)
      dispatches++;
    if(a->flags & ActionFlags::Clear)
      clears++;
    if(a->flags & ActionFlags::Copy)
      copies++;
    if(a->flags & ActionFlags::PushMarker)
      markers++;
  }

  Json &counts = ret["counts"];
  counts["actions"] = (uint64_t)m_Linear.size();
  counts["rootActions"] = (uint64_t)m_Ctrl->GetRootActions().size();
  counts["draws"] = draws;
  counts["dispatches"] = dispatches;
  counts["clears"] = clears;
  counts["copies"] = copies;
  counts["markerRegions"] = markers;
  counts["events"] = m_LastEvent;
  counts["resources"] = (uint64_t)m_Ctrl->GetResources().size();
  counts["textures"] = (uint64_t)m_Ctrl->GetTextures().size();
  counts["buffers"] = (uint64_t)m_Ctrl->GetBuffers().size();
  counts["debugMessages"] = (uint64_t)m_Messages.size();

  Json &features = ret["features"];
  features["shaderDebugging"] = m_Props.shaderDebugging;
  features["pixelHistory"] = m_Props.pixelHistory;

  ret["currentEvent"] = m_CurEvent;
  return ret;
}

Json McpServer::ActionRow(const ActionDescription &action, int depth) const
{
  Json row = Json::object();
  row["eventId"] = action.eventId;
  if(depth >= 0)
    row["depth"] = depth;
  row["name"] = ActionName(action);
  row["flags"] = str(ToStr(action.flags));
  if(action.flags & ActionFlags::Drawcall)
  {
    row["numIndices"] = action.numIndices;
    if(action.numInstances > 1)
      row["numInstances"] = action.numInstances;
  }
  if(action.flags & ActionFlags::Dispatch)
  {
    Json dims = Json::array();
    for(uint32_t d : action.dispatchDimension)
      dims.push_back(d);
    row["dispatch"] = dims;
  }
  if(!action.children.empty())
    row["children"] = (uint64_t)action.children.size();
  return row;
}

Json McpServer::SDToJson(const SDObject *obj, int depth, uint32_t maxArray) const
{
  if(!obj)
    return Json();

  switch(obj->type.basetype)
  {
    case SDBasic::Chunk:
    case SDBasic::Struct:
    {
      if(depth <= 0)
        return Json("{...}");
      Json ret = Json::object();
      for(size_t i = 0; i < obj->NumChildren(); i++)
      {
        const SDObject *child = obj->GetChild(i);
        std::string key = child ? str(child->name) : std::string();
        if(key.empty())
          key = "[" + std::to_string((uint64_t)i) + "]";
        if(ret.has(key.c_str()))
          key += "#" + std::to_string((uint64_t)i);
        ret[key] = SDToJson(child, depth - 1, maxArray);
      }
      return ret;
    }
    case SDBasic::Array:
    {
      if(depth <= 0)
        return Json("[" + std::to_string((uint64_t)obj->NumChildren()) + " elements]");
      Json ret = Json::array();
      size_t count = obj->NumChildren();
      for(size_t i = 0; i < count && i < maxArray; i++)
        ret.push_back(SDToJson(obj->GetChild(i), depth - 1, maxArray));
      if(count > maxArray)
        ret.push_back("... " + std::to_string((uint64_t)(count - maxArray)) + " more elements");
      return ret;
    }
    case SDBasic::Null: return Json();
    case SDBasic::Buffer:
      return Json("<buffer, " + std::to_string((uint64_t)obj->type.byteSize) + " bytes>");
    case SDBasic::String: return Json(str(obj->AsString()));
    case SDBasic::Enum:
    {
      std::string s = str(obj->AsString());
      if(!s.empty())
        return Json(s);
      return Json(obj->AsUInt64());
    }
    case SDBasic::UnsignedInteger: return Json(obj->AsUInt64());
    case SDBasic::SignedInteger: return Json(obj->AsInt64());
    case SDBasic::Float: return Json(obj->AsDouble());
    case SDBasic::Boolean: return Json(obj->AsBool());
    case SDBasic::Character: return Json(std::string(1, obj->AsChar()));
    case SDBasic::Resource:
    {
      ResourceId id = obj->AsResourceId();
      if(id == ResourceId())
        return Json("ResourceId::0 (null)");
      std::string s = "ResourceId::" + std::to_string(IdNum(id));
      std::string name = ResName(id);
      if(!name.empty())
        s += " (" + name + ")";
      return Json(s);
    }
    case SDBasic::GPUAddress: return Json(obj->AsUInt64());
  }

  return Json();
}

Json McpServer::DescriptorJson(const Descriptor &desc, bool includeType) const
{
  Json ret = Json::object();
  if(includeType)
    ret["type"] = str(ToStr(desc.type));
  ret["resource"] = ResRef(desc.resource);
  if(desc.view != ResourceId())
    ret["view"] = IdNum(desc.view);
  bool hasFormat = desc.format.type == ResourceFormatType::Regular
                       ? desc.format.compCount > 0
                       : desc.format.type != ResourceFormatType::Undefined;
  if(hasFormat)
    ret["format"] = str(desc.format.Name());
  if(desc.textureType != TextureType::Unknown)
  {
    ret["textureType"] = str(ToStr(desc.textureType));
    if(desc.firstMip != 0 || desc.numMips != 1)
    {
      ret["firstMip"] = (uint32_t)desc.firstMip;
      ret["numMips"] = (uint32_t)desc.numMips;
    }
    if(desc.firstSlice != 0 || desc.numSlices != 1)
    {
      ret["firstSlice"] = (uint32_t)desc.firstSlice;
      ret["numSlices"] = (uint32_t)desc.numSlices;
    }
  }
  else if(desc.byteSize != 0 || desc.byteOffset != 0)
  {
    ret["byteOffset"] = desc.byteOffset;
    ret["byteSize"] = desc.byteSize;
  }
  return ret;
}

Json McpServer::UsedDescriptorJson(const UsedDescriptor &used, const ShaderReflection *refl) const
{
  Json ret = Json::object();
  const DescriptorAccess &access = used.access;

  if(access.index != DescriptorAccess::NoShaderBinding)
  {
    ret["index"] = (uint32_t)access.index;
    if(refl)
    {
      std::string name;
      switch(CategoryForDescriptorType(access.type))
      {
        case DescriptorCategory::ConstantBlock:
          if(access.index < refl->constantBlocks.size())
            name = str(refl->constantBlocks[access.index].name);
          break;
        case DescriptorCategory::ReadOnlyResource:
          if(access.index < refl->readOnlyResources.size())
            name = str(refl->readOnlyResources[access.index].name);
          break;
        case DescriptorCategory::ReadWriteResource:
          if(access.index < refl->readWriteResources.size())
            name = str(refl->readWriteResources[access.index].name);
          break;
        case DescriptorCategory::Sampler:
          if(access.index < refl->samplers.size())
            name = str(refl->samplers[access.index].name);
          break;
        default: break;
      }
      if(!name.empty())
        ret["name"] = name;
    }
  }
  if(access.arrayElement != 0)
    ret["arrayElement"] = access.arrayElement;
  if(access.staticallyUnused)
    ret["staticallyUnused"] = true;

  if(CategoryForDescriptorType(access.type) == DescriptorCategory::Sampler)
  {
    ret["type"] = str(ToStr(access.type));
    ret["filter"] = str(ToStr(used.sampler.filter.minify)) + "/" +
                    str(ToStr(used.sampler.filter.magnify)) + "/" +
                    str(ToStr(used.sampler.filter.mip));
    ret["addressU"] = str(ToStr(used.sampler.addressU));
    ret["addressV"] = str(ToStr(used.sampler.addressV));
    ret["addressW"] = str(ToStr(used.sampler.addressW));
  }
  else
  {
    Json desc = DescriptorJson(used.descriptor);
    for(const auto &kv : desc.members())
      ret[kv.first] = kv.second;
  }
  return ret;
}

Json McpServer::ShaderVariableJson(const ShaderVariable &var) const
{
  if(!var.members.empty())
  {
    // array elements are named like [0] or name[0] - emit those as JSON arrays
    std::string first = str(var.members[0].name);
    bool isArray = first == "[0]" || first == str(var.name) + "[0]";
    if(isArray)
    {
      Json ret = Json::array();
      for(const ShaderVariable &m : var.members)
        ret.push_back(ShaderVariableJson(m));
      return ret;
    }
    Json ret = Json::object();
    for(const ShaderVariable &m : var.members)
      ret[str(m.name)] = ShaderVariableJson(m);
    return ret;
  }

  uint32_t count = std::max(1U, uint32_t(var.rows) * uint32_t(var.columns));
  count = std::min(count, 16U);

  Json values = Json::array();
  for(uint32_t i = 0; i < count; i++)
  {
    switch(var.type)
    {
      case VarType::Float: values.push_back(var.value.f32v[i]); break;
      case VarType::Double: values.push_back(var.value.f64v[i]); break;
      case VarType::Half: values.push_back((float)var.value.f16v[i]); break;
      case VarType::SInt: values.push_back((int64_t)var.value.s32v[i]); break;
      case VarType::UInt: values.push_back(var.value.u32v[i]); break;
      case VarType::SShort: values.push_back((int64_t)var.value.s16v[i]); break;
      case VarType::UShort: values.push_back((uint32_t)var.value.u16v[i]); break;
      case VarType::SLong: values.push_back(var.value.s64v[i]); break;
      case VarType::ULong: values.push_back(var.value.u64v[i]); break;
      case VarType::SByte: values.push_back((int64_t)var.value.s8v[i]); break;
      case VarType::UByte: values.push_back((uint32_t)var.value.u8v[i]); break;
      case VarType::Bool: values.push_back(var.value.u32v[i] != 0); break;
      case VarType::Enum: values.push_back(var.value.u32v[i]); break;
      case VarType::GPUPointer: values.push_back(var.value.u64v[i]); break;
      default: values.push_back(var.value.u32v[i]); break;
    }
  }

  if(count == 1)
    return values.items().front();

  // present matrices as an array of rows
  if(var.rows > 1 && var.columns > 1)
  {
    Json rows = Json::array();
    for(uint32_t r = 0; r < var.rows; r++)
    {
      Json row = Json::array();
      for(uint32_t c = 0; c < var.columns && r * var.columns + c < count; c++)
        row.push_back(values.items()[r * var.columns + c]);
      rows.push_back(row);
    }
    return rows;
  }

  return values;
}

///////////////////////////////////////////////////////////////////////////////
// tools

McpServer::ToolResult McpServer::ToolOpenCapture(const Json &args)
{
  std::string path = RequireString(args, "path");
  OpenCaptureInternal(path);
  return TextResult(CaptureSummary());
}

McpServer::ToolResult McpServer::ToolCloseCapture(const Json &)
{
  bool wasOpen = m_Ctrl != NULL;
  CloseCapture();
  return TextResult(Json(wasOpen ? "Capture closed." : "No capture was open."));
}

McpServer::ToolResult McpServer::ToolCaptureInfo(const Json &)
{
  RequireCapture();
  PullDebugMessages();
  Json ret = CaptureSummary();

  FrameDescription frame = m_Ctrl->GetFrameInfo();
  if(frame.stats.recorded)
  {
    Json &stats = ret["stats"];
    stats["draws"] = frame.stats.draws.calls;
    stats["instancedDraws"] = frame.stats.draws.instanced;
    stats["indirectDraws"] = frame.stats.draws.indirect;
    stats["dispatches"] = frame.stats.dispatches.calls;
    stats["indirectDispatches"] = frame.stats.dispatches.indirect;
    stats["resourceUpdates"] = frame.stats.updates.calls;
  }

  Json swaps = Json::array();
  for(const TextureDescription &tex : m_Ctrl->GetTextures())
  {
    if(tex.creationFlags & TextureCategory::SwapBuffer)
    {
      Json t = ResRef(tex.resourceId);
      t["width"] = tex.width;
      t["height"] = tex.height;
      t["format"] = str(tex.format.Name());
      swaps.push_back(t);
    }
  }
  ret["swapchainImages"] = swaps;

  Json roots = Json::array();
  for(const ActionDescription &a : m_Ctrl->GetRootActions())
  {
    if(roots.size() >= 50)
    {
      roots.push_back("... use list_actions for more");
      break;
    }
    roots.push_back(ActionRow(a, -1));
  }
  ret["rootActions"] = roots;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolListActions(const Json &args)
{
  RequireCapture();

  std::string filter = ArgString(args, "filter");
  std::vector<ActionFlags> flagFilters;

  if(HasArg(args, "flags"))
  {
    std::vector<std::string> names;
    const Json &f = Arg(args, "flags");
    if(f.isArray())
    {
      for(const Json &n : f.items())
        names.push_back(n.asString());
    }
    else
    {
      // allow comma or pipe separated
      std::string s = f.asString();
      size_t start = 0;
      while(start <= s.size())
      {
        size_t end = s.find_first_of(",|", start);
        if(end == std::string::npos)
          end = s.size();
        std::string tok = s.substr(start, end - start);
        tok.erase(std::remove(tok.begin(), tok.end(), ' '), tok.end());
        if(!tok.empty())
          names.push_back(tok);
        start = end + 1;
      }
    }

    for(const std::string &n : names)
    {
      bool found = false;
      for(uint32_t bit = 0; bit < 32; bit++)
      {
        ActionFlags flag = ActionFlags(1U << bit);
        if(lower(str(ToStr(flag))) == lower(n))
        {
          flagFilters.push_back(flag);
          found = true;
          break;
        }
      }
      if(!found)
        throw ToolError("Unknown action flag '" + n +
                        "'. Common flags: Drawcall, Dispatch, Clear, Copy, Resolve, Present, "
                        "PushMarker, SetMarker, PassBoundary, BeginPass, EndPass, Indexed, "
                        "Instanced, Indirect, MeshDispatch, DispatchRay");
    }
  }

  bool searching = !filter.empty() || !flagFilters.empty();
  int maxDepth = (int)ArgInt(args, "max_depth", searching ? 1000 : 1);
  uint64_t offset = ArgUInt(args, "offset", 0);
  uint64_t limit = std::min<uint64_t>(ArgUInt(args, "limit", 100), 2000);

  const rdcarray<ActionDescription> *list = &m_Ctrl->GetRootActions();
  if(HasArg(args, "parent_event_id"))
  {
    uint32_t parentId = (uint32_t)ArgUInt(args, "parent_event_id", 0);
    auto it = m_ActionIndex.find(parentId);
    if(it == m_ActionIndex.end())
      throw ToolError("No action has eventId " + std::to_string(parentId));
    list = &m_Linear[it->second]->children;
  }

  Json rows = Json::array();
  uint64_t total = 0;

  std::function<void(const rdcarray<ActionDescription> &, int)> walk =
      [&](const rdcarray<ActionDescription> &actions, int depth) {
        for(const ActionDescription &a : actions)
        {
          bool match = true;
          if(!filter.empty() && !containsNoCase(ActionName(a), filter))
            match = false;
          if(match && !flagFilters.empty())
          {
            match = false;
            for(ActionFlags f : flagFilters)
              if(a.flags & f)
                match = true;
          }

          if(match)
          {
            if(total >= offset && rows.size() < limit)
              rows.push_back(ActionRow(a, depth));
            total++;
          }

          if(depth + 1 < maxDepth)
            walk(a.children, depth + 1);
        }
      };

  walk(*list, 0);

  Json ret = Json::object();
  ret["total"] = total;
  ret["offset"] = offset;
  ret["returned"] = (uint64_t)rows.size();
  if(offset + rows.size() < total)
    ret["nextOffset"] = offset + rows.size();
  ret["actions"] = rows;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolGetAction(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  const ActionDescription *action = ActionForEvent(eventId);
  if(!action)
    throw ToolError("No action at event " + std::to_string(eventId));

  Json ret = ActionRow(*action, -1);
  if(eventId != action->eventId)
    ret["requestedEventId"] = eventId;
  ret["actionId"] = action->actionId;

  std::vector<const ActionDescription *> parents;
  for(const ActionDescription *p = m_Parent.at(action); p; p = m_Parent.at(p))
    parents.push_back(p);
  Json path = Json::array();
  for(auto it = parents.rbegin(); it != parents.rend(); ++it)
  {
    Json entry = Json::object();
    entry["eventId"] = (*it)->eventId;
    entry["name"] = ActionName(**it);
    path.push_back(entry);
  }
  ret["markerPath"] = path;

  if(action->flags & ActionFlags::Drawcall)
  {
    Json &draw = ret["draw"];
    draw["numIndices"] = action->numIndices;
    draw["numInstances"] = action->numInstances;
    draw["baseVertex"] = (int64_t)action->baseVertex;
    draw["indexOffset"] = action->indexOffset;
    draw["vertexOffset"] = action->vertexOffset;
    draw["instanceOffset"] = action->instanceOffset;
    draw["drawIndex"] = action->drawIndex;
  }
  if(action->flags & (ActionFlags::Dispatch | ActionFlags::MeshDispatch))
  {
    Json &dispatch = ret["dispatch"];
    Json groups = Json::array(), threads = Json::array(), base = Json::array();
    for(int i = 0; i < 3; i++)
    {
      groups.push_back(action->dispatchDimension[i]);
      threads.push_back(action->dispatchThreadsDimension[i]);
      base.push_back(action->dispatchBase[i]);
    }
    dispatch["groups"] = groups;
    dispatch["threadsPerGroup"] = threads;
    dispatch["base"] = base;
  }

  Json outputs = Json::array();
  for(ResourceId out : action->outputs)
    if(out != ResourceId())
      outputs.push_back(ResRef(out));
  if(outputs.size() > 0)
    ret["outputs"] = outputs;
  if(action->depthOut != ResourceId())
    ret["depthOutput"] = ResRef(action->depthOut);
  if(action->copySource != ResourceId())
    ret["copySource"] = ResRef(action->copySource);
  if(action->copyDestination != ResourceId())
    ret["copyDestination"] = ResRef(action->copyDestination);

  const SDFile &sdfile = m_Ctrl->GetStructuredFile();
  Json events = Json::array();
  for(const APIEvent &ev : action->events)
  {
    Json e = Json::object();
    e["eventId"] = ev.eventId;
    if(ev.chunkIndex < sdfile.chunks.size())
      e["call"] = str(sdfile.chunks[ev.chunkIndex]->name);
    events.push_back(e);
  }
  ret["apiEvents"] = events;

  auto it = m_ActionIndex.find(action->eventId);
  if(it != m_ActionIndex.end())
  {
    if(it->second > 0)
      ret["previousEventId"] = m_Linear[it->second - 1]->eventId;
    if(it->second + 1 < m_Linear.size())
      ret["nextEventId"] = m_Linear[it->second + 1]->eventId;
  }

  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolGetApiCalls(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  const ActionDescription *action = ActionForEvent(eventId);
  if(!action)
    throw ToolError("No action at event " + std::to_string(eventId));

  int depth = (int)ArgInt(args, "max_depth", 6);
  uint32_t maxArray = (uint32_t)ArgUInt(args, "max_array", 32);
  bool onlyThis = ArgBool(args, "only_this_event", false);

  const SDFile &sdfile = m_Ctrl->GetStructuredFile();
  Json calls = Json::array();
  for(const APIEvent &ev : action->events)
  {
    if(onlyThis && ev.eventId != eventId)
      continue;
    Json call = Json::object();
    call["eventId"] = ev.eventId;
    if(ev.chunkIndex < sdfile.chunks.size())
    {
      const SDChunk *chunk = sdfile.chunks[ev.chunkIndex];
      call["call"] = str(chunk->name);
      if(chunk->metadata.durationMicro >= 0)
        call["durationMicro"] = chunk->metadata.durationMicro;
      call["params"] = SDToJson(chunk, depth, maxArray);
    }
    calls.push_back(call);
  }

  Json ret = Json::object();
  ret["action"] = ActionRow(*action, -1);
  ret["calls"] = calls;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolPipelineState(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  bool includeUnused = ArgBool(args, "include_unused", false);
  uint32_t maxBindings = (uint32_t)ArgUInt(args, "max_bindings", 64);

  const PipeState &ps = m_Ctrl->GetPipelineState();
  const ActionDescription *action = ActionForEvent(eventId);

  Json ret = Json::object();
  ret["eventId"] = eventId;
  if(action)
    ret["action"] = ActionName(*action);
  ret["api"] = str(ToStr(m_Props.pipelineType));

  bool isCompute = action && (action->flags & ActionFlags::Dispatch);

  std::vector<ShaderStage> stages;
  if(!isCompute)
    stages.insert(stages.end(), std::begin(GraphicsStages), std::end(GraphicsStages));
  stages.push_back(ShaderStage::Compute);

  Json shaders = Json::array();
  for(ShaderStage stage : stages)
  {
    ResourceId shader = ps.GetShader(stage);
    if(shader == ResourceId())
      continue;
    Json s = ResRef(shader);
    s["stage"] = str(ToStr(stage));
    s["entryPoint"] = str(ps.GetShaderEntryPoint(stage));
    shaders.push_back(s);
  }
  ret["shaders"] = shaders;

  if(ps.GetGraphicsPipelineObject() != ResourceId())
    ret["graphicsPipeline"] = ResRef(ps.GetGraphicsPipelineObject());
  if(ps.GetComputePipelineObject() != ResourceId())
    ret["computePipeline"] = ResRef(ps.GetComputePipelineObject());

  if(!isCompute)
  {
    Json &ia = ret["inputAssembly"];
    ia["topology"] = str(ToStr(ps.GetPrimitiveTopology()));
    BoundVBuffer ib = ps.GetIBuffer();
    if(ib.resourceId != ResourceId())
    {
      Json ibj = ResRef(ib.resourceId);
      ibj["byteOffset"] = ib.byteOffset;
      ibj["byteStride"] = ib.byteStride;
      if(ib.byteSize != ~0ULL)
        ibj["byteSize"] = ib.byteSize;
      ia["indexBuffer"] = ibj;
      if(ps.IsRestartEnabled())
        ia["restartIndex"] = ps.GetRestartIndex();
    }

    Json vbs = Json::array();
    rdcarray<BoundVBuffer> vbuffers = ps.GetVBuffers();
    for(size_t i = 0; i < vbuffers.size(); i++)
    {
      if(vbuffers[i].resourceId == ResourceId())
        continue;
      Json vb = ResRef(vbuffers[i].resourceId);
      vb["slot"] = (uint64_t)i;
      vb["byteOffset"] = vbuffers[i].byteOffset;
      vb["byteStride"] = vbuffers[i].byteStride;
      // ~0 means 'to the end of the buffer'
      if(vbuffers[i].byteSize != ~0ULL)
        vb["byteSize"] = vbuffers[i].byteSize;
      vbs.push_back(vb);
    }
    ia["vertexBuffers"] = vbs;

    Json attrs = Json::array();
    for(const VertexInputAttribute &attr : ps.GetVertexInputs())
    {
      if(!attr.used && !includeUnused)
        continue;
      Json a = Json::object();
      a["name"] = str(attr.name);
      a["vertexBuffer"] = (int64_t)attr.vertexBuffer;
      a["byteOffset"] = attr.byteOffset;
      a["format"] = str(attr.format.Name());
      if(attr.perInstance)
        a["instanceRate"] = (int64_t)attr.instanceRate;
      if(attr.genericEnabled)
        a["generic"] = true;
      attrs.push_back(a);
    }
    ia["attributes"] = attrs;

    Viewport vp = ps.GetViewport(0);
    Json &vpj = ret["viewport"];
    vpj["x"] = vp.x;
    vpj["y"] = vp.y;
    vpj["width"] = vp.width;
    vpj["height"] = vp.height;
    vpj["minDepth"] = vp.minDepth;
    vpj["maxDepth"] = vp.maxDepth;

    Scissor sc = ps.GetScissor(0);
    if(sc.enabled)
    {
      Json &scj = ret["scissor"];
      scj["x"] = (int64_t)sc.x;
      scj["y"] = (int64_t)sc.y;
      scj["width"] = (int64_t)sc.width;
      scj["height"] = (int64_t)sc.height;
    }

    RasterState rs = ps.GetRasterState();
    Json &rsj = ret["rasterizer"];
    rsj["fillMode"] = str(ToStr(rs.fillMode));
    rsj["cullMode"] = str(ToStr(rs.cullMode));
    rsj["frontCCW"] = rs.frontCCW;

    DepthTestState ds = ps.GetDepthTestState();
    Json &dsj = ret["depthStencil"];
    dsj["depthTest"] = ds.depthEnable;
    if(ds.depthEnable)
      dsj["depthFunction"] = str(ToStr(ds.depthFunction));
    dsj["depthWrites"] = ds.depthWrites;
    if(ds.depthBounds)
    {
      dsj["depthBoundsMin"] = ds.minDepthBounds;
      dsj["depthBoundsMax"] = ds.maxDepthBounds;
    }
    dsj["stencilTest"] = ps.IsStencilTestEnabled();
    if(ps.IsStencilTestEnabled())
    {
      rdcpair<StencilFace, StencilFace> faces = ps.GetStencilFaces();
      const StencilFace *f[2] = {&faces.first, &faces.second};
      const char *names[2] = {"front", "back"};
      for(int i = 0; i < 2; i++)
      {
        Json &fj = dsj[names[i]];
        fj["function"] = str(ToStr(f[i]->function));
        fj["failOp"] = str(ToStr(f[i]->failOperation));
        fj["depthFailOp"] = str(ToStr(f[i]->depthFailOperation));
        fj["passOp"] = str(ToStr(f[i]->passOperation));
        fj["reference"] = f[i]->reference;
        fj["compareMask"] = f[i]->compareMask;
        fj["writeMask"] = f[i]->writeMask;
      }
    }

    rdcarray<Descriptor> targets = ps.GetOutputTargets();
    rdcarray<ColorBlend> blends = ps.GetColorBlends();
    Json outs = Json::array();
    for(size_t i = 0; i < targets.size(); i++)
    {
      if(targets[i].resource == ResourceId())
        continue;
      Json t = DescriptorJson(targets[i], false);
      t["slot"] = (uint64_t)i;
      if(i < blends.size())
      {
        const ColorBlend &b = blends[i];
        Json &bj = t["blend"];
        bj["enabled"] = b.enabled;
        if(b.enabled)
        {
          bj["color"] = str(ToStr(b.colorBlend.source)) + " " + str(ToStr(b.colorBlend.operation)) +
                        " " + str(ToStr(b.colorBlend.destination));
          bj["alpha"] = str(ToStr(b.alphaBlend.source)) + " " + str(ToStr(b.alphaBlend.operation)) +
                        " " + str(ToStr(b.alphaBlend.destination));
        }
        if(b.logicOperationEnabled)
          bj["logicOp"] = str(ToStr(b.logicOperation));
        bj["writeMask"] = (uint32_t)b.writeMask;
      }
      outs.push_back(t);
    }
    ret["colorTargets"] = outs;

    Descriptor depth = ps.GetDepthTarget();
    if(depth.resource != ResourceId())
      ret["depthTarget"] = DescriptorJson(depth, false);
    Descriptor depthResolve = ps.GetDepthResolveTarget();
    if(depthResolve.resource != ResourceId())
      ret["depthResolveTarget"] = DescriptorJson(depthResolve, false);
  }

  Json bindings = Json::object();
  for(ShaderStage stage : stages)
  {
    if(ps.GetShader(stage) == ResourceId())
      continue;
    const ShaderReflection *refl = ps.GetShaderReflection(stage);

    Json stageJson = Json::object();
    auto addList = [&](const char *key, const rdcarray<UsedDescriptor> &list) {
      if(list.empty())
        return;
      Json arr = Json::array();
      for(size_t i = 0; i < list.size(); i++)
      {
        if(i >= maxBindings)
        {
          arr.push_back("... " + std::to_string((uint64_t)(list.size() - maxBindings)) +
                        " more (raise max_bindings)");
          break;
        }
        arr.push_back(UsedDescriptorJson(list[i], refl));
      }
      stageJson[key] = arr;
    };

    addList("constantBlocks", ps.GetConstantBlocks(stage, !includeUnused));
    addList("readOnly", ps.GetReadOnlyResources(stage, !includeUnused));
    addList("readWrite", ps.GetReadWriteResources(stage, !includeUnused));
    addList("samplers", ps.GetSamplers(stage, !includeUnused));

    if(stageJson.size() > 0)
      bindings[str(ToStr(stage))] = stageJson;
  }
  ret["bindings"] = bindings;

  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolGetShader(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  ShaderStage stage = ParseStage(RequireString(args, "stage"));

  const PipeState &ps = m_Ctrl->GetPipelineState();
  const ShaderReflection *refl = ps.GetShaderReflection(stage);
  if(!refl)
    throw ToolError("No " + str(ToStr(stage)) + " shader is bound at event " +
                    std::to_string(eventId));

  Json ret = ResRef(ps.GetShader(stage));
  ret["stage"] = str(ToStr(stage));
  ret["entryPoint"] = str(refl->entryPoint);
  ret["encoding"] = str(ToStr(refl->encoding));
  if(stage == ShaderStage::Compute || stage == ShaderStage::Task || stage == ShaderStage::Mesh)
  {
    Json dims = Json::array();
    for(uint32_t d : refl->dispatchThreadsDimension)
      dims.push_back(d);
    ret["threadsPerGroup"] = dims;
  }

  auto sigJson = [&](const rdcarray<SigParameter> &sig) {
    Json arr = Json::array();
    for(const SigParameter &p : sig)
    {
      Json s = Json::object();
      if(!p.varName.empty())
        s["name"] = str(p.varName);
      if(!p.semanticIdxName.empty())
        s["semantic"] = str(p.semanticIdxName);
      s["register"] = p.regIndex;
      if(p.systemValue != ShaderBuiltin::Undefined)
        s["builtin"] = str(ToStr(p.systemValue));
      s["type"] = str(ToStr(p.varType));
      s["components"] = p.compCount;
      arr.push_back(s);
    }
    return arr;
  };
  ret["inputs"] = sigJson(refl->inputSignature);
  ret["outputs"] = sigJson(refl->outputSignature);

  std::function<Json(const rdcarray<ShaderConstant> &, int)> constantsJson =
      [&](const rdcarray<ShaderConstant> &vars, int depth) {
        Json arr = Json::array();
        for(const ShaderConstant &c : vars)
        {
          Json v = Json::object();
          v["name"] = str(c.name);
          v["offset"] = c.byteOffset;
          v["type"] = str(c.type.name);
          if(c.type.elements > 1)
            v["elements"] = c.type.elements;
          if(!c.type.members.empty() && depth > 0)
            v["members"] = constantsJson(c.type.members, depth - 1);
          arr.push_back(v);
        }
        return arr;
      };

  Json cbs = Json::array();
  for(size_t i = 0; i < refl->constantBlocks.size(); i++)
  {
    const ConstantBlock &cb = refl->constantBlocks[i];
    Json c = Json::object();
    c["index"] = (uint64_t)i;
    c["name"] = str(cb.name);
    c["byteSize"] = cb.byteSize;
    c["bind"] = cb.fixedBindNumber;
    c["space"] = cb.fixedBindSetOrSpace;
    if(cb.bindArraySize > 1)
      c["arraySize"] = cb.bindArraySize;
    c["variables"] = constantsJson(cb.variables, 3);
    cbs.push_back(c);
  }
  ret["constantBlocks"] = cbs;

  auto resJson = [&](const rdcarray<ShaderResource> &list) {
    Json arr = Json::array();
    for(size_t i = 0; i < list.size(); i++)
    {
      const ShaderResource &r = list[i];
      Json j = Json::object();
      j["index"] = (uint64_t)i;
      j["name"] = str(r.name);
      j["descriptorType"] = str(ToStr(r.descriptorType));
      if(r.isTexture)
        j["textureType"] = str(ToStr(r.textureType));
      j["bind"] = r.fixedBindNumber;
      j["space"] = r.fixedBindSetOrSpace;
      if(r.bindArraySize > 1)
        j["arraySize"] = r.bindArraySize;
      arr.push_back(j);
    }
    return arr;
  };
  ret["readOnlyResources"] = resJson(refl->readOnlyResources);
  ret["readWriteResources"] = resJson(refl->readWriteResources);

  Json samplers = Json::array();
  for(size_t i = 0; i < refl->samplers.size(); i++)
  {
    Json j = Json::object();
    j["index"] = (uint64_t)i;
    j["name"] = str(refl->samplers[i].name);
    j["bind"] = refl->samplers[i].fixedBindNumber;
    j["space"] = refl->samplers[i].fixedBindSetOrSpace;
    samplers.push_back(j);
  }
  ret["samplers"] = samplers;

  const ShaderDebugInfo &dbg = refl->debugInfo;
  Json files = Json::array();
  for(const ShaderSourceFile &f : dbg.files)
    files.push_back(str(f.filename));
  if(files.size() > 0)
  {
    ret["sourceFiles"] = files;
    ret["compiler"] = str(ToStr(dbg.compiler));
  }
  ret["debuggable"] = dbg.debuggable;

  ResourceId pipe = stage == ShaderStage::Compute ? ps.GetComputePipelineObject()
                                                  : ps.GetGraphicsPipelineObject();

  rdcarray<rdcstr> targets = m_Ctrl->GetDisassemblyTargets(true);
  Json targetList = Json::array();
  for(const rdcstr &t : targets)
    targetList.push_back(str(t));
  ret["disassemblyTargets"] = targetList;

  if(ArgBool(args, "include_disassembly", true))
  {
    std::string target = ArgString(args, "disassembly_target");
    if(target.empty() && !targets.empty())
      target = str(targets[0]);
    ret["disassemblyTarget"] = target;
    ret["disassembly"] = str(m_Ctrl->DisassembleShader(pipe, refl, conv(target)));
  }

  if(ArgBool(args, "include_source", false))
  {
    Json sources = Json::array();
    for(const ShaderSourceFile &f : dbg.files)
    {
      Json s = Json::object();
      s["filename"] = str(f.filename);
      s["contents"] = str(f.contents);
      sources.push_back(s);
    }
    ret["sources"] = sources;
  }

  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolGetCBuffer(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  ShaderStage stage = ParseStage(RequireString(args, "stage"));
  uint32_t index = (uint32_t)ArgUInt(args, "index", 0);
  uint32_t arrayIdx = (uint32_t)ArgUInt(args, "array_index", 0);

  const PipeState &ps = m_Ctrl->GetPipelineState();
  const ShaderReflection *refl = ps.GetShaderReflection(stage);
  if(!refl)
    throw ToolError("No " + str(ToStr(stage)) + " shader is bound at event " +
                    std::to_string(eventId));
  if(index >= refl->constantBlocks.size())
    throw ToolError("Constant block index " + std::to_string(index) + " out of range - the " +
                    str(ToStr(stage)) + " shader has " +
                    std::to_string((uint64_t)refl->constantBlocks.size()) +
                    " constant blocks (see get_shader).");

  UsedDescriptor cb = ps.GetConstantBlock(stage, index, arrayIdx);
  ResourceId pipe = stage == ShaderStage::Compute ? ps.GetComputePipelineObject()
                                                  : ps.GetGraphicsPipelineObject();

  rdcarray<ShaderVariable> vars = m_Ctrl->GetCBufferVariableContents(
      pipe, refl->resourceId, stage, ps.GetShaderEntryPoint(stage), index, cb.descriptor.resource,
      cb.descriptor.byteOffset, cb.descriptor.byteSize);

  Json ret = Json::object();
  ret["eventId"] = eventId;
  ret["stage"] = str(ToStr(stage));
  ret["index"] = index;
  ret["name"] = str(refl->constantBlocks[index].name);
  if(cb.descriptor.resource != ResourceId())
  {
    ret["buffer"] = ResRef(cb.descriptor.resource);
    ret["byteOffset"] = cb.descriptor.byteOffset;
    ret["byteSize"] = cb.descriptor.byteSize;
  }
  Json values = Json::object();
  for(const ShaderVariable &v : vars)
    values[str(v.name)] = ShaderVariableJson(v);
  ret["values"] = values;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolListResources(const Json &args)
{
  RequireCapture();
  std::string typeFilter = lower(ArgString(args, "type"));
  std::string nameFilter = ArgString(args, "filter");
  bool includeUnnamed = ArgBool(args, "include_unnamed", true);
  uint64_t offset = ArgUInt(args, "offset", 0);
  uint64_t limit = std::min<uint64_t>(ArgUInt(args, "limit", 100), 2000);

  Json rows = Json::array();
  uint64_t total = 0;
  for(const ResourceDescription &res : m_Ctrl->GetResources())
  {
    std::string type = str(ToStr(res.type));
    // 'texture' and 'buffer' match anything with texture/buffer contents, e.g. swapchain images
    if(typeFilter == "texture")
    {
      if(!FindTexture(res.resourceId))
        continue;
    }
    else if(typeFilter == "buffer")
    {
      if(!FindBuffer(res.resourceId))
        continue;
    }
    else if(!typeFilter.empty() && lower(type) != typeFilter)
    {
      continue;
    }
    std::string name = str(res.name);
    if(!containsNoCase(name, nameFilter))
      continue;
    if(!includeUnnamed && res.autogeneratedName)
      continue;

    if(total >= offset && rows.size() < limit)
    {
      Json r = Json::object();
      r["id"] = IdNum(res.resourceId);
      r["name"] = name;
      r["type"] = type;
      if(const TextureDescription *tex = FindTexture(res.resourceId))
      {
        r["textureType"] = str(ToStr(tex->type));
        r["width"] = tex->width;
        if(tex->height > 1)
          r["height"] = tex->height;
        if(tex->depth > 1)
          r["depth"] = tex->depth;
        if(tex->arraysize > 1)
          r["arraySize"] = tex->arraysize;
        if(tex->mips > 1)
          r["mips"] = tex->mips;
        if(tex->msSamp > 1)
          r["samples"] = tex->msSamp;
        r["format"] = str(tex->format.Name());
      }
      else if(const BufferDescription *buf = FindBuffer(res.resourceId))
      {
        r["byteSize"] = buf->length;
      }
      rows.push_back(r);
    }
    total++;
  }

  Json ret = Json::object();
  ret["total"] = total;
  ret["offset"] = offset;
  ret["returned"] = (uint64_t)rows.size();
  if(offset + rows.size() < total)
    ret["nextOffset"] = offset + rows.size();
  ret["resources"] = rows;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolGetResource(const Json &args)
{
  RequireCapture();
  ResourceId id;
  if(!ParseResourceId(Arg(args, "resource_id"), id))
    throw ToolError("Missing or invalid resource_id - pass the integer resource ID.");

  const ResourceDescription *desc = NULL;
  for(const ResourceDescription &res : m_Ctrl->GetResources())
    if(res.resourceId == id)
      desc = &res;
  if(!desc)
    throw ToolError("No resource with ID " + std::to_string(IdNum(id)));

  Json ret = Json::object();
  ret["id"] = IdNum(id);
  ret["name"] = str(desc->name);
  ret["type"] = str(ToStr(desc->type));

  Json parents = Json::array();
  for(ResourceId p : desc->parentResources)
    parents.push_back(ResRef(p));
  if(parents.size() > 0)
    ret["parents"] = parents;
  Json derived = Json::array();
  for(ResourceId d : desc->derivedResources)
    derived.push_back(ResRef(d));
  if(derived.size() > 0)
    ret["derived"] = derived;

  if(const TextureDescription *tex = FindTexture(id))
  {
    Json &t = ret["texture"];
    t["type"] = str(ToStr(tex->type));
    t["format"] = str(tex->format.Name());
    t["width"] = tex->width;
    t["height"] = tex->height;
    t["depth"] = tex->depth;
    t["mips"] = tex->mips;
    t["arraySize"] = tex->arraysize;
    t["samples"] = tex->msSamp;
    t["cubemap"] = tex->cubemap;
    t["byteSize"] = tex->byteSize;
    t["usageFlags"] = str(ToStr(tex->creationFlags));
  }
  if(const BufferDescription *buf = FindBuffer(id))
  {
    Json &b = ret["buffer"];
    b["byteSize"] = buf->length;
    b["usageFlags"] = str(ToStr(buf->creationFlags));
    if(buf->gpuAddress)
      b["gpuAddress"] = buf->gpuAddress;
  }

  uint64_t usageLimit = ArgUInt(args, "usage_limit", 100);
  rdcarray<EventUsage> usage = m_Ctrl->GetUsage(id);
  Json uses = Json::array();
  for(size_t i = 0; i < usage.size() && i < usageLimit; i++)
  {
    Json u = Json::object();
    u["eventId"] = usage[i].eventId;
    u["usage"] = str(ToStr(usage[i].usage));
    uses.push_back(u);
  }
  ret["usageCount"] = (uint64_t)usage.size();
  ret["usage"] = uses;

  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolViewTexture(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  ResourceId id = ResolveTexture(args, eventId);
  const TextureDescription *tex = FindTexture(id);

  TextureSave save;
  save.resourceId = id;
  save.destType = FileType::PNG;
  save.mip = (int32_t)ArgInt(args, "mip", 0);
  save.slice.sliceIndex = (int32_t)ArgInt(args, "slice", 0);
  if(HasArg(args, "sample"))
    save.sample.sampleIndex = (uint32_t)ArgUInt(args, "sample", 0);
  save.comp.blackPoint = (float)ArgDouble(args, "range_min", 0.0);
  save.comp.whitePoint = (float)ArgDouble(args, "range_max", 1.0);
  save.alpha = ArgBool(args, "keep_alpha", false) ? AlphaMapping::Preserve : AlphaMapping::Discard;

  std::string channel = lower(ArgString(args, "channel"));
  if(channel == "r" || channel == "red")
    save.channelExtract = 0;
  else if(channel == "g" || channel == "green")
    save.channelExtract = 1;
  else if(channel == "b" || channel == "blue")
    save.channelExtract = 2;
  else if(channel == "a" || channel == "alpha")
    save.channelExtract = 3;

  std::string tmp = TempFilePath(".png");
  ResultDetails res = m_Ctrl->SaveTexture(save, conv(tmp));
  if(!res.OK())
  {
    remove(tmp.c_str());
    throw ToolError("Failed to read back texture: " + str(res.Message()));
  }

  std::vector<unsigned char> png;
  bool read = ReadWholeFile(tmp, png);
  remove(tmp.c_str());
  if(!read || png.empty())
    throw ToolError("Failed to read back texture data");

  int maxDim = (int)std::max<int64_t>(16, ArgInt(args, "max_size", 1024));
  int w = 0, h = 0;
  DownscalePNG(png, maxDim, w, h);

  Json meta = ResRef(id);
  meta["eventId"] = eventId;
  if(tex)
  {
    meta["format"] = str(tex->format.Name());
    meta["width"] = std::max(1U, tex->width >> std::max(0, save.mip));
    meta["height"] = std::max(1U, tex->height >> std::max(0, save.mip));
  }
  meta["mip"] = (int64_t)save.mip;
  meta["slice"] = (int64_t)save.slice.sliceIndex;
  if(w > 0)
  {
    meta["imageWidth"] = (int64_t)w;
    meta["imageHeight"] = (int64_t)h;
  }
  meta["range"] = Json::array();
  meta["range"].push_back(save.comp.blackPoint);
  meta["range"].push_back(save.comp.whitePoint);

  ToolResult ret = TextResult(meta);
  Json image = Json::object();
  image["type"] = "image";
  image["data"] = Base64(png);
  image["mimeType"] = "image/png";
  ret.content.push_back(image);
  return ret;
}

McpServer::ToolResult McpServer::ToolSaveTexture(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  ResourceId id = ResolveTexture(args, eventId);
  std::string path = RequireString(args, "path");

  std::string format = lower(ArgString(args, "format"));
  if(format.empty())
  {
    size_t dot = path.rfind('.');
    if(dot != std::string::npos)
      format = lower(path.substr(dot + 1));
  }

  TextureSave save;
  save.resourceId = id;
  if(format == "png")
    save.destType = FileType::PNG;
  else if(format == "jpg" || format == "jpeg")
    save.destType = FileType::JPG;
  else if(format == "bmp")
    save.destType = FileType::BMP;
  else if(format == "tga")
    save.destType = FileType::TGA;
  else if(format == "hdr")
    save.destType = FileType::HDR;
  else if(format == "exr")
    save.destType = FileType::EXR;
  else if(format == "dds")
    save.destType = FileType::DDS;
  else if(format == "raw")
    save.destType = FileType::Raw;
  else
    throw ToolError("Unknown format '" + format +
                    "'. Use png, jpg, bmp, tga, hdr, exr, dds or raw.");

  // DDS keeps all mips/slices by default, other formats save a single subresource
  save.mip = (int32_t)ArgInt(args, "mip", save.destType == FileType::DDS ? -1 : 0);
  save.slice.sliceIndex = (int32_t)ArgInt(args, "slice", save.destType == FileType::DDS ? -1 : 0);
  save.alpha = ArgBool(args, "keep_alpha", true) ? AlphaMapping::Preserve : AlphaMapping::Discard;

  ResultDetails res = m_Ctrl->SaveTexture(save, conv(path));
  if(!res.OK())
    throw ToolError("Failed to save texture: " + str(res.Message()));

  Json ret = ResRef(id);
  ret["eventId"] = eventId;
  ret["path"] = path;
  ret["format"] = format;
  return TextResult(ret);
}

static Json PixelValueJson(const PixelValue &val)
{
  Json ret = Json::object();
  Json f = Json::array(), u = Json::array(), i = Json::array();
  for(int c = 0; c < 4; c++)
  {
    f.push_back(val.floatValue[c]);
    u.push_back(val.uintValue[c]);
    i.push_back((int64_t)val.intValue[c]);
  }
  ret["float"] = f;
  ret["uint"] = u;
  ret["int"] = i;
  return ret;
}

McpServer::ToolResult McpServer::ToolPickPixel(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  SetEvent(eventId);
  ResourceId id = ResolveTexture(args, eventId);
  if(!HasArg(args, "x") || !HasArg(args, "y"))
    throw ToolError("x and y are required");
  uint32_t x = (uint32_t)ArgUInt(args, "x", 0);
  uint32_t y = (uint32_t)ArgUInt(args, "y", 0);

  Subresource sub;
  Json subJson = SubresourceArgs(args, sub);

  PixelValue val = m_Ctrl->PickPixel(id, x, y, sub, CompType::Typeless);

  Json ret = ResRef(id);
  ret["eventId"] = eventId;
  ret["x"] = x;
  ret["y"] = y;
  ret["subresource"] = subJson;
  if(const TextureDescription *tex = FindTexture(id))
    ret["format"] = str(tex->format.Name());
  ret["value"] = PixelValueJson(val);
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolPixelHistory(const Json &args)
{
  uint32_t eventId = ResolveEvent(args);
  if(!m_Props.pixelHistory)
    throw ToolError("Pixel history is not supported for this capture's API on this replay.");
  SetEvent(eventId);
  ResourceId id = ResolveTexture(args, eventId);
  if(!HasArg(args, "x") || !HasArg(args, "y"))
    throw ToolError("x and y are required");
  uint32_t x = (uint32_t)ArgUInt(args, "x", 0);
  uint32_t y = (uint32_t)ArgUInt(args, "y", 0);
  uint64_t limit = ArgUInt(args, "limit", 100);

  Subresource sub;
  Json subJson = SubresourceArgs(args, sub);

  rdcarray<PixelModification> history = m_Ctrl->PixelHistory(id, x, y, sub, CompType::Typeless);

  Json mods = Json::array();
  for(size_t i = 0; i < history.size() && i < limit; i++)
  {
    const PixelModification &m = history[i];
    Json mj = Json::object();
    mj["eventId"] = m.eventId;
    if(const ActionDescription *a = ActionForEvent(m.eventId))
      mj["action"] = ActionName(*a);
    if(m.directShaderWrite)
      mj["directShaderWrite"] = true;
    mj["fragment"] = m.fragIndex;
    mj["primitive"] = m.primitiveID;
    mj["before"] = PixelValueJson(m.preMod.col)["float"];
    mj["shaderOutput"] = PixelValueJson(m.shaderOut.col)["float"];
    mj["after"] = PixelValueJson(m.postMod.col)["float"];
    mj["depthBefore"] = m.preMod.depth;
    mj["depthAfter"] = m.postMod.depth;

    Json failed = Json::array();
    if(m.sampleMasked)
      failed.push_back("sampleMasked");
    if(m.backfaceCulled)
      failed.push_back("backfaceCulled");
    if(m.depthClipped)
      failed.push_back("depthClipped");
    if(m.depthBoundsFailed)
      failed.push_back("depthBoundsFailed");
    if(m.viewClipped)
      failed.push_back("viewClipped");
    if(m.scissorClipped)
      failed.push_back("scissorClipped");
    if(m.shaderDiscarded)
      failed.push_back("shaderDiscarded");
    if(m.depthTestFailed)
      failed.push_back("depthTestFailed");
    if(m.stencilTestFailed)
      failed.push_back("stencilTestFailed");
    if(m.predicationSkipped)
      failed.push_back("predicationSkipped");
    mj["passed"] = failed.size() == 0;
    if(failed.size() > 0)
      mj["rejectedBy"] = failed;
    mods.push_back(mj);
  }

  Json ret = ResRef(id);
  ret["x"] = x;
  ret["y"] = y;
  ret["subresource"] = subJson;
  ret["upToEventId"] = eventId;
  ret["total"] = (uint64_t)history.size();
  ret["modifications"] = mods;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolBufferData(const Json &args)
{
  RequireCapture();
  ResourceId id;
  if(!ParseResourceId(Arg(args, "resource_id"), id))
    throw ToolError("Missing or invalid resource_id - pass the integer resource ID.");
  const BufferDescription *buf = FindBuffer(id);
  if(!buf)
    throw ToolError("Resource " + std::to_string(IdNum(id)) + " is not a buffer.");

  if(HasArg(args, "event_id"))
    SetEvent(ResolveEvent(args));

  uint64_t offset = ArgUInt(args, "offset", 0);
  uint64_t length = std::min<uint64_t>(ArgUInt(args, "length", 256), 64 * 1024);
  std::string format = lower(ArgString(args, "format", "hex"));

  bytebuf data = m_Ctrl->GetBufferData(id, offset, length);

  Json ret = ResRef(id);
  ret["eventId"] = m_CurEvent;
  ret["bufferSize"] = buf->length;
  ret["offset"] = offset;
  ret["length"] = (uint64_t)data.size();
  ret["format"] = format;

  if(format == "hex")
  {
    // 16 bytes per line, prefixed by the offset
    Json lines = Json::array();
    for(size_t i = 0; i < data.size(); i += 16)
    {
      char line[128];
      int len = snprintf(line, sizeof(line), "%08llx:", (unsigned long long)(offset + i));
      for(size_t j = i; j < i + 16 && j < data.size(); j++)
        len += snprintf(line + len, sizeof(line) - len, " %02x", data[j]);
      lines.push_back(std::string(line));
    }
    ret["data"] = lines;
  }
  else
  {
    Json values = Json::array();
    size_t stride = 4;
    if(format == "uint16" || format == "int16" || format == "half")
      stride = 2;
    else if(format == "uint8" || format == "int8")
      stride = 1;
    else if(format == "float64" || format == "double" || format == "uint64" || format == "int64")
      stride = 8;
    else if(format != "float" && format != "float32" && format != "uint" && format != "uint32" &&
            format != "int" && format != "int32")
      throw ToolError("Unknown format '" + format +
                      "'. Use hex, float32, float64, half, uint32, int32, uint16, int16, uint8, "
                      "int8, uint64 or int64.");

    for(size_t i = 0; i + stride <= data.size(); i += stride)
    {
      const byte *p = data.data() + i;
      if(format == "float" || format == "float32")
      {
        float f;
        memcpy(&f, p, 4);
        values.push_back(f);
      }
      else if(format == "uint" || format == "uint32")
      {
        uint32_t v;
        memcpy(&v, p, 4);
        values.push_back(v);
      }
      else if(format == "int" || format == "int32")
      {
        int32_t v;
        memcpy(&v, p, 4);
        values.push_back((int64_t)v);
      }
      else if(format == "half")
      {
        uint16_t v;
        memcpy(&v, p, 2);
        values.push_back(RENDERDOC_HalfToFloat(v));
      }
      else if(format == "uint16")
      {
        uint16_t v;
        memcpy(&v, p, 2);
        values.push_back((uint32_t)v);
      }
      else if(format == "int16")
      {
        int16_t v;
        memcpy(&v, p, 2);
        values.push_back((int64_t)v);
      }
      else if(format == "uint8")
      {
        values.push_back((uint32_t)p[0]);
      }
      else if(format == "int8")
      {
        values.push_back((int64_t)(int8_t)p[0]);
      }
      else if(format == "float64" || format == "double")
      {
        double v;
        memcpy(&v, p, 8);
        values.push_back(v);
      }
      else if(format == "uint64")
      {
        uint64_t v;
        memcpy(&v, p, 8);
        values.push_back(v);
      }
      else if(format == "int64")
      {
        int64_t v;
        memcpy(&v, p, 8);
        values.push_back(v);
      }
    }
    ret["data"] = values;
  }

  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolDebugMessages(const Json &args)
{
  RequireCapture();
  PullDebugMessages();

  std::string minSeverity = lower(ArgString(args, "min_severity"));
  int threshold = 3;
  if(minSeverity == "high")
    threshold = 0;
  else if(minSeverity == "medium")
    threshold = 1;
  else if(minSeverity == "low")
    threshold = 2;
  else if(!minSeverity.empty() && minSeverity != "info")
    throw ToolError("min_severity must be one of: high, medium, low, info");

  uint64_t offset = ArgUInt(args, "offset", 0);
  uint64_t limit = std::min<uint64_t>(ArgUInt(args, "limit", 100), 1000);

  Json rows = Json::array();
  uint64_t total = 0;
  for(const DebugMessage &msg : m_Messages)
  {
    // MessageSeverity is ordered High=0, Medium, Low, Info
    if((int)msg.severity > threshold)
      continue;
    if(total >= offset && rows.size() < limit)
    {
      Json m = Json::object();
      m["eventId"] = msg.eventId;
      m["severity"] = str(ToStr(msg.severity));
      m["category"] = str(ToStr(msg.category));
      m["source"] = str(ToStr(msg.source));
      m["messageId"] = msg.messageID;
      m["description"] = str(msg.description);
      rows.push_back(m);
    }
    total++;
  }

  Json ret = Json::object();
  ret["total"] = total;
  ret["offset"] = offset;
  ret["returned"] = (uint64_t)rows.size();
  if(offset + rows.size() < total)
    ret["nextOffset"] = offset + rows.size();
  ret["messages"] = rows;
  return TextResult(ret);
}

McpServer::ToolResult McpServer::ToolCaptureProgram(const Json &args)
{
  std::string exe = RequireString(args, "executable");
  std::string cmdLine = ArgString(args, "arguments");
  std::string workingDir = ArgString(args, "working_dir");
  std::string captureTemplate = ArgString(args, "capture_path_template");
  double delay = std::max(0.0, ArgDouble(args, "delay_seconds", 5.0));
  uint32_t numFrames = (uint32_t)std::max<uint64_t>(1, ArgUInt(args, "num_frames", 1));
  double timeout = std::max(5.0, ArgDouble(args, "timeout_seconds", 60.0));
  bool open = ArgBool(args, "open", true);

  CaptureOptions opts;
  opts.apiValidation = ArgBool(args, "api_validation", false);
  opts.debugOutputMute = !opts.apiValidation;
  opts.captureCallstacks = ArgBool(args, "capture_callstacks", false);
  opts.refAllResources = ArgBool(args, "ref_all_resources", false);
  opts.hookIntoChildren = ArgBool(args, "hook_children", false);

  rdcarray<EnvironmentModification> env;
  ExecuteResult exec = RENDERDOC_ExecuteAndInject(conv(exe), conv(workingDir), conv(cmdLine), env,
                                                  conv(captureTemplate), opts, false);
  if(exec.result.code != ResultCode::Succeeded)
    throw ToolError("Failed to launch and inject into '" + exe + "': " + str(exec.result.Message()));

  ITargetControl *control = RENDERDOC_CreateTargetControl("", exec.ident, "renderdoccmd mcp", true);
  if(!control)
    throw ToolError("Launched '" + exe + "' but couldn't connect to it (ident " +
                    std::to_string(exec.ident) + ").");

  typedef std::chrono::steady_clock clock;
  auto start = clock::now();
  auto elapsed = [&]() { return std::chrono::duration<double>(clock::now() - start).count(); };

  std::string api;
  bool triggered = false;
  std::vector<NewCaptureData> captures;
  std::string failure;

  while(true)
  {
    TargetControlMessage msg = control->ReceiveMessage(NULL);
    if(msg.type == TargetControlMessageType::Disconnected)
    {
      failure = "The application disconnected (exited?) before a capture was made.";
      break;
    }
    if(msg.type == TargetControlMessageType::RegisterAPI)
      api = str(msg.apiUse.name);
    if(msg.type == TargetControlMessageType::NewCapture)
    {
      captures.push_back(msg.newCapture);
      if(captures.size() >= numFrames)
        break;
    }

    if(!triggered && elapsed() >= delay)
    {
      control->TriggerCapture(numFrames);
      triggered = true;
    }

    if(elapsed() >= delay + timeout)
    {
      failure = api.empty()
                    ? "Timed out - the application never initialised a supported graphics API."
                    : "Timed out waiting for a capture. The application might not be presenting "
                      "frames.";
      break;
    }

    if(msg.type == TargetControlMessageType::Noop)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  uint32_t pid = control->GetPID();
  std::string target = str(control->GetTarget());
  control->Shutdown();

  if(captures.empty())
    throw ToolError(failure);

  Json ret = Json::object();
  ret["target"] = target;
  ret["pid"] = pid;
  if(!api.empty())
    ret["api"] = api;
  Json caps = Json::array();
  for(const NewCaptureData &c : captures)
  {
    Json cj = Json::object();
    cj["path"] = str(c.path);
    cj["frameNumber"] = c.frameNumber;
    cj["byteSize"] = c.byteSize;
    caps.push_back(cj);
  }
  ret["captures"] = caps;
  ret["note"] = "The application is still running. Close it when you're finished with it.";

  if(open)
  {
    std::string path = str(captures.back().path);
    OpenCaptureInternal(path);
    ret["opened"] = CaptureSummary();
  }

  return TextResult(ret);
}

///////////////////////////////////////////////////////////////////////////////
// tool registration

void McpServer::RegisterTools()
{
  using namespace std::placeholders;
#define BIND(fn) std::bind(&McpServer::fn, this, _1)

  AddTool("open_capture", "Open capture",
          "Open a RenderDoc capture (.rdc) file for replay and analysis. Any previously open "
          "capture is closed. Returns a summary: API, action/draw counts, resource counts.",
          MakeSchema({{"path", "string", "Absolute path to the .rdc capture file.", true}}), true,
          BIND(ToolOpenCapture));

  AddTool("close_capture", "Close capture",
          "Close the currently open capture and free its replay resources.", MakeSchema({}), true,
          BIND(ToolCloseCapture));

  AddTool("get_capture_info", "Capture info",
          "Summary of the open capture: API, GPU vendor, frame statistics, swapchain images, "
          "debug message count and the top-level actions.",
          MakeSchema({}), true, BIND(ToolCaptureInfo));

  AddTool(
      "list_actions", "List actions",
      "List actions (draws, dispatches, clears, copies, marker regions) in the frame as rows of "
      "{eventId, depth, name, flags}. Without filters returns the children of parent_event_id "
      "(or the root) down to max_depth (default 1). With a name filter or flags it searches the "
      "whole subtree. Paginate with offset/limit.",
      MakeSchema({
          {"parent_event_id", "integer", "List children of this marker/action. Omit for root.", false},
          {"max_depth", "integer",
           "How many levels to descend (default 1, or unlimited when "
           "filtering).",
           false},
          {"filter", "string", "Case-insensitive substring to match against action names.", false},
          {"flags", "string",
           "Only actions with any of these flags, comma separated, e.g. 'Drawcall,Dispatch'.", false},
          {"offset", "integer", "Skip this many matching rows (default 0).", false},
          {"limit", "integer", "Maximum rows to return (default 100, max 2000).", false},
      }),
      true, BIND(ToolListActions));

  AddTool("get_action", "Get action",
          "Details for the action containing event_id: draw/dispatch parameters, output "
          "targets, marker path, API calls that make it up, and previous/next action IDs.",
          MakeSchema({{"event_id", "integer", "Event ID (defaults to the current event).", false}}),
          true, BIND(ToolGetAction));

  AddTool("get_api_calls", "Get API calls",
          "The API calls (with all their parameters, as structured data) recorded for the action "
          "containing event_id - e.g. the state-setting calls leading up to a draw.",
          MakeSchema({
              {"event_id", "integer", "Event ID (defaults to the current event).", false},
              {"only_this_event", "boolean", "Only return the call at exactly event_id.", false},
              {"max_depth", "integer", "Maximum nesting depth of parameters (default 6).", false},
              {"max_array", "integer", "Maximum array elements shown (default 32).", false},
          }),
          true, BIND(ToolGetApiCalls));

  AddTool(
      "get_pipeline_state", "Get pipeline state",
      "The full GPU pipeline state at event_id: bound shaders, vertex/index buffers and "
      "attributes, viewport/scissor, rasterizer, depth/stencil, blend, render targets and, per "
      "shader stage, the bound constant buffers, textures/buffers (read-only and read-write) "
      "and samplers.",
      MakeSchema({
          {"event_id", "integer", "Event ID (defaults to the current event).", false},
          {"include_unused", "boolean",
           "Include bindings/attributes the shaders don't statically use (default false).", false},
          {"max_bindings", "integer", "Max bindings per category per stage (default 64).", false},
      }),
      true, BIND(ToolPipelineState));

  AddTool(
      "get_shader", "Get shader",
      "Reflection and disassembly of the shader bound to a stage at event_id: input/output "
      "signatures, constant block layouts, resource and sampler bindings, embedded source files.",
      MakeSchema({
          {"stage", "string",
           "Shader stage: vertex, hull, domain, geometry, pixel, compute, task or mesh.", true},
          {"event_id", "integer", "Event ID (defaults to the current event).", false},
          {"include_disassembly", "boolean", "Include disassembly text (default true).", false},
          {"disassembly_target", "string",
           "Disassembly format, from disassemblyTargets (default: the native one).", false},
          {"include_source", "boolean",
           "Include embedded shader source code if the capture has debug info (default false).", false},
      }),
      true, BIND(ToolGetShader));

  AddTool("get_cbuffer_contents", "Get constant buffer contents",
          "The values of all variables in a constant/uniform buffer bound to a shader stage at "
          "event_id, decoded using shader reflection.",
          MakeSchema({
              {"stage", "string", "Shader stage, e.g. vertex or pixel.", true},
              {"index", "integer",
               "Constant block index in the shader's reflection (see get_shader constantBlocks). "
               "Default 0.",
               false},
              {"array_index", "integer", "Element if the binding is an array (default 0).", false},
              {"event_id", "integer", "Event ID (defaults to the current event).", false},
          }),
          true, BIND(ToolGetCBuffer));

  AddTool(
      "list_resources", "List resources",
      "List resources in the capture (textures, buffers, shaders, pipelines, ...) with "
      "IDs, names and, for textures/buffers, dimensions and formats.",
      MakeSchema({
          {"type", "string",
           "Only this resource type, e.g. Texture, Buffer, Shader, PipelineState, Sampler.", false},
          {"filter", "string", "Case-insensitive substring to match against names.", false},
          {"include_unnamed", "boolean",
           "Include resources with auto-generated names (default true).", false},
          {"offset", "integer", "Skip this many rows (default 0).", false},
          {"limit", "integer", "Maximum rows (default 100, max 2000).", false},
      }),
      true, BIND(ToolListResources));

  AddTool("get_resource", "Get resource",
          "Details of one resource: texture/buffer properties, parent/derived resources and every "
          "event where it is used (and how).",
          MakeSchema({
              {"resource_id", "integer", "Resource ID.", true},
              {"usage_limit", "integer", "Max usage entries to list (default 100).", false},
          }),
          true, BIND(ToolGetResource));

  AddTool(
      "view_texture", "View texture",
      "Render a texture or render target as it is at event_id and return it as a PNG image. "
      "Defaults to the action's first colour output (or depth, or the backbuffer). Use "
      "range_min/range_max to remap values (e.g. for depth or HDR) and channel to isolate one.",
      MakeSchema({
          {"resource_id", "integer", "Texture resource ID (default: the action's output).", false},
          {"event_id", "integer", "Event ID (defaults to the current event).", false},
          {"mip", "integer", "Mip level (default 0).", false},
          {"slice", "integer", "Array slice / cube face / 3D slice (default 0).", false},
          {"sample", "integer", "MSAA sample (default: resolve).", false},
          {"range_min", "number", "Value mapped to black (default 0).", false},
          {"range_max", "number", "Value mapped to white (default 1).", false},
          {"channel", "string", "Show a single channel: r, g, b or a.", false},
          {"keep_alpha", "boolean", "Preserve alpha instead of discarding it (default false).", false},
          {"max_size", "integer", "Downscale so neither dimension exceeds this (default 1024).", false},
      }),
      true, BIND(ToolViewTexture));

  AddTool(
      "save_texture", "Save texture",
      "Save a texture as it is at event_id to a file on disk (png, jpg, bmp, tga, hdr, exr, "
      "dds or raw, chosen from the extension or format).",
      MakeSchema({
          {"path", "string", "Destination file path.", true},
          {"resource_id", "integer", "Texture resource ID (default: the action's output).", false},
          {"event_id", "integer", "Event ID (defaults to the current event).", false},
          {"format", "string", "File format (default: from the path's extension).", false},
          {"mip", "integer", "Mip level (default 0, or all for DDS).", false},
          {"slice", "integer", "Array slice (default 0, or all for DDS).", false},
          {"keep_alpha", "boolean", "Preserve alpha (default true).", false},
      }),
      false, BIND(ToolSaveTexture));

  AddTool(
      "pick_pixel", "Pick pixel",
      "Read the exact value of one texel of a texture at event_id, as float, uint and int.",
      MakeSchema({
          {"x", "integer", "X coordinate.", true},
          {"y", "integer", "Y coordinate.", true},
          {"resource_id", "integer", "Texture resource ID (default: the action's output).", false},
          {"event_id", "integer", "Event ID (defaults to the current event).", false},
          {"mip", "integer", "Mip level (default 0).", false},
          {"slice", "integer", "Array slice (default 0).", false},
          {"sample", "integer", "MSAA sample (default 0).", false},
      }),
      true, BIND(ToolPickPixel));

  AddTool(
      "pixel_history", "Pixel history",
      "Every event up to event_id that touched one pixel of a texture: the value before and "
      "after, the shader output, and which tests (depth, stencil, culling, scissor, discard...) "
      "rejected each fragment. The go-to tool for 'why is this pixel this colour?'.",
      MakeSchema({
          {"x", "integer", "X coordinate.", true},
          {"y", "integer", "Y coordinate.", true},
          {"resource_id", "integer", "Texture resource ID (default: the action's output).", false},
          {"event_id", "integer", "History up to this event (defaults to the current event).", false},
          {"mip", "integer", "Mip level (default 0).", false},
          {"slice", "integer", "Array slice (default 0).", false},
          {"sample", "integer", "MSAA sample (default 0).", false},
          {"limit", "integer", "Max modifications listed (default 100).", false},
      }),
      true, BIND(ToolPixelHistory));

  AddTool("get_buffer_data", "Get buffer data",
          "Read raw bytes from a buffer (as of event_id if given) and show them as hex or as "
          "typed values.",
          MakeSchema({
              {"resource_id", "integer", "Buffer resource ID.", true},
              {"offset", "integer", "Byte offset (default 0).", false},
              {"length", "integer", "Bytes to read (default 256, max 65536).", false},
              {"format", "string",
               "hex (default), float32, float64, half, uint32, int32, uint16, int16, uint8, int8, "
               "uint64 or int64.",
               false},
              {"event_id", "integer", "Read contents as of this event.", false},
          }),
          true, BIND(ToolBufferData));

  AddTool("get_debug_messages", "Get debug messages",
          "API validation / debug messages recorded in the capture or raised during replay.",
          MakeSchema({
              {"min_severity", "string", "high, medium, low or info (default: all).", false},
              {"offset", "integer", "Skip this many messages (default 0).", false},
              {"limit", "integer", "Maximum messages (default 100).", false},
          }),
          true, BIND(ToolDebugMessages));

  AddTool(
      "capture_program", "Capture program",
      "Launch an application with RenderDoc injected, wait delay_seconds, capture num_frames "
      "frames and (by default) open the resulting capture for analysis. The application keeps "
      "running afterwards.",
      MakeSchema({
          {"executable", "string", "Path to the executable to launch.", true},
          {"arguments", "string", "Command line arguments for the application.", false},
          {"working_dir", "string", "Working directory (default: the executable's directory).", false},
          {"delay_seconds", "number", "Seconds to wait before capturing (default 5).", false},
          {"num_frames", "integer", "Number of consecutive frames to capture (default 1).", false},
          {"timeout_seconds", "number",
           "Seconds to wait for the capture after triggering (default 60).", false},
          {"capture_path_template", "string",
           "Path template for capture files (default: RenderDoc's temp folder).", false},
          {"api_validation", "boolean", "Enable API validation layers (default false).", false},
          {"capture_callstacks", "boolean", "Capture CPU callstacks (default false).", false},
          {"ref_all_resources", "boolean", "Include all live resources (default false).", false},
          {"hook_children", "boolean", "Also inject into child processes (default false).", false},
          {"open", "boolean", "Open the capture for analysis afterwards (default true).", false},
      }),
      false, BIND(ToolCaptureProgram));

#undef BIND
}

///////////////////////////////////////////////////////////////////////////////
// command

int g_McpStdout = -1;

struct McpCommand : public Command
{
  std::string initialCapture;

  virtual void AddOptions(cmdline::parser &parser) { parser.set_footer("[capture.rdc]"); }
  virtual const char *Description()
  {
    return "Run a Model Context Protocol (MCP) server over stdio for AI-assisted capture analysis.";
  }
  virtual bool IsInternalOnly() { return false; }
  virtual bool IsCaptureCommand() { return false; }
  virtual bool Parse(cmdline::parser &parser, GlobalEnvironment &)
  {
    std::vector<std::string> rest = parser.rest();
    if(!rest.empty())
    {
      initialCapture = rest[0];
      rest.erase(rest.begin());
      parser.set_rest(rest);
    }

    // stdout carries the protocol, so everything else that would print there (including log
    // output from the replay library) must go to stderr. Keep a private handle to the real stdout
    // and point fd 1 at stderr before replay is initialised.
    fflush(stdout);
#if defined(_WIN32)
    g_McpStdout = _dup(_fileno(stdout));
    _dup2(_fileno(stderr), _fileno(stdout));
    _setmode(g_McpStdout, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#else
    g_McpStdout = dup(fileno(stdout));
    dup2(fileno(stderr), fileno(stdout));
#endif
    return g_McpStdout >= 0;
  }
  virtual int Execute(const CaptureOptions &)
  {
#if defined(_WIN32)
    FILE *out = _fdopen(g_McpStdout, "wb");
#else
    FILE *out = fdopen(g_McpStdout, "wb");
#endif
    if(!out)
    {
      std::cerr << "Couldn't open protocol output stream" << std::endl;
      return 1;
    }

    std::cerr << "RenderDoc MCP server " << FULL_VERSION_STRING << " ready on stdio" << std::endl;

    int ret = 0;
    {
      McpServer server(out);

      if(!initialCapture.empty())
      {
        try
        {
          server.OpenCaptureInternal(initialCapture);
          std::cerr << "Opened '" << initialCapture << "'" << std::endl;
        }
        catch(std::exception &e)
        {
          std::cerr << "Couldn't open initial capture: " << e.what() << std::endl;
        }
      }

      ret = server.Run(std::cin);
    }

    fclose(out);
    return ret;
  }
};
};    // anonymous namespace

Command *MakeMcpCommand()
{
  return new McpCommand();
}
