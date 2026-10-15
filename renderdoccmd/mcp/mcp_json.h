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

#pragma once

// A minimal self-contained JSON value, parser and writer used by the MCP server. Objects keep
// their insertion order so that tool output reads naturally.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace mcp
{
class Json
{
public:
  enum class Type
  {
    Null,
    Bool,
    Int,
    UInt,
    Double,
    String,
    Array,
    Object,
  };

  Json() : m_Type(Type::Null) {}
  Json(std::nullptr_t) : m_Type(Type::Null) {}
  Json(bool b) : m_Type(Type::Bool) { m_Bool = b; }
  Json(int v) : m_Type(Type::Int) { m_Int = v; }
  Json(int64_t v) : m_Type(Type::Int) { m_Int = v; }
  Json(uint32_t v) : m_Type(Type::UInt) { m_UInt = v; }
  Json(uint64_t v) : m_Type(Type::UInt) { m_UInt = v; }
  Json(float v) : m_Type(Type::Double) { m_Double = v; }
  Json(double v) : m_Type(Type::Double) { m_Double = v; }
  Json(const char *s) : m_Type(Type::String), m_Str(s ? s : "") {}
  Json(const std::string &s) : m_Type(Type::String), m_Str(s) {}
  Json(std::string &&s) : m_Type(Type::String), m_Str(std::move(s)) {}

  static Json array() { return Json(Type::Array); }
  static Json object() { return Json(Type::Object); }

  Type type() const { return m_Type; }
  bool isNull() const { return m_Type == Type::Null; }
  bool isBool() const { return m_Type == Type::Bool; }
  bool isNumber() const
  {
    return m_Type == Type::Int || m_Type == Type::UInt || m_Type == Type::Double;
  }
  bool isString() const { return m_Type == Type::String; }
  bool isArray() const { return m_Type == Type::Array; }
  bool isObject() const { return m_Type == Type::Object; }

  bool asBool(bool def = false) const { return m_Type == Type::Bool ? m_Bool : def; }
  double asDouble(double def = 0.0) const
  {
    switch(m_Type)
    {
      case Type::Int: return (double)m_Int;
      case Type::UInt: return (double)m_UInt;
      case Type::Double: return m_Double;
      default: return def;
    }
  }
  int64_t asInt(int64_t def = 0) const
  {
    switch(m_Type)
    {
      case Type::Int: return m_Int;
      case Type::UInt: return (int64_t)m_UInt;
      case Type::Double: return (int64_t)m_Double;
      default: return def;
    }
  }
  uint64_t asUInt(uint64_t def = 0) const
  {
    switch(m_Type)
    {
      case Type::Int: return m_Int < 0 ? def : (uint64_t)m_Int;
      case Type::UInt: return m_UInt;
      case Type::Double: return m_Double < 0.0 ? def : (uint64_t)m_Double;
      default: return def;
    }
  }
  const std::string &asString() const { return m_Str; }

  // array access
  size_t size() const
  {
    if(m_Type == Type::Array)
      return m_Arr.size();
    if(m_Type == Type::Object)
      return m_Obj.size();
    return 0;
  }
  const Json &operator[](size_t i) const { return i < m_Arr.size() ? m_Arr[i] : nullValue(); }
  Json &push_back(Json v)
  {
    if(m_Type != Type::Array)
      *this = array();
    m_Arr.push_back(std::move(v));
    return m_Arr.back();
  }
  const std::vector<Json> &items() const { return m_Arr; }

  // object access
  const std::vector<std::pair<std::string, Json>> &members() const { return m_Obj; }
  bool has(const char *key) const { return find(key) != NULL; }
  const Json *find(const char *key) const
  {
    if(m_Type != Type::Object)
      return NULL;
    for(const auto &kv : m_Obj)
      if(kv.first == key)
        return &kv.second;
    return NULL;
  }
  const Json &operator[](const char *key) const
  {
    const Json *ret = find(key);
    return ret ? *ret : nullValue();
  }
  Json &operator[](const char *key)
  {
    if(m_Type != Type::Object)
      *this = object();
    for(auto &kv : m_Obj)
      if(kv.first == key)
        return kv.second;
    m_Obj.push_back(std::make_pair(std::string(key), Json()));
    return m_Obj.back().second;
  }
  Json &operator[](const std::string &key) { return (*this)[key.c_str()]; }
  const Json &operator[](const std::string &key) const { return (*this)[key.c_str()]; }

  std::string dump() const
  {
    std::string ret;
    write(ret);
    return ret;
  }

  // returns false and fills in error on failure
  static bool parse(const std::string &text, Json &out, std::string &error)
  {
    Parser p(text);
    p.skipWS();
    if(!p.parseValue(out, 0))
    {
      error = p.error;
      return false;
    }
    p.skipWS();
    if(p.pos != text.size())
    {
      error = "Unexpected trailing characters at offset " + std::to_string(p.pos);
      return false;
    }
    return true;
  }

private:
  explicit Json(Type t) : m_Type(t) {}

  static const Json &nullValue()
  {
    static const Json n;
    return n;
  }

  static void writeString(std::string &out, const std::string &s)
  {
    out.push_back('"');
    for(size_t i = 0; i < s.size(); i++)
    {
      unsigned char c = (unsigned char)s[i];
      switch(c)
      {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if(c < 0x20)
          {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          }
          else
          {
            out.push_back((char)c);
          }
          break;
      }
    }
    out.push_back('"');
  }

  void write(std::string &out) const
  {
    switch(m_Type)
    {
      case Type::Null: out += "null"; break;
      case Type::Bool: out += m_Bool ? "true" : "false"; break;
      case Type::Int: out += std::to_string(m_Int); break;
      case Type::UInt: out += std::to_string(m_UInt); break;
      case Type::Double:
      {
        if(std::isnan(m_Double) || std::isinf(m_Double))
        {
          // JSON has no representation for these, emit them as strings
          out += std::isnan(m_Double) ? "\"NaN\"" : (m_Double > 0 ? "\"Inf\"" : "\"-Inf\"");
        }
        else
        {
          char buf[32];
          snprintf(buf, sizeof(buf), "%.9g", m_Double);
          out += buf;
        }
        break;
      }
      case Type::String: writeString(out, m_Str); break;
      case Type::Array:
      {
        out.push_back('[');
        for(size_t i = 0; i < m_Arr.size(); i++)
        {
          if(i > 0)
            out.push_back(',');
          m_Arr[i].write(out);
        }
        out.push_back(']');
        break;
      }
      case Type::Object:
      {
        out.push_back('{');
        for(size_t i = 0; i < m_Obj.size(); i++)
        {
          if(i > 0)
            out.push_back(',');
          writeString(out, m_Obj[i].first);
          out.push_back(':');
          m_Obj[i].second.write(out);
        }
        out.push_back('}');
        break;
      }
    }
  }

  struct Parser
  {
    explicit Parser(const std::string &t) : text(t) {}

    const std::string &text;
    size_t pos = 0;
    std::string error;

    bool fail(const char *msg)
    {
      if(error.empty())
        error = std::string(msg) + " at offset " + std::to_string(pos);
      return false;
    }

    void skipWS()
    {
      while(pos < text.size() &&
            (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
        pos++;
    }

    bool literal(const char *lit)
    {
      size_t len = strlen(lit);
      if(text.compare(pos, len, lit) != 0)
        return false;
      pos += len;
      return true;
    }

    bool parseValue(Json &out, int depth)
    {
      if(depth > 256)
        return fail("Nesting too deep");

      if(pos >= text.size())
        return fail("Unexpected end of input");

      char c = text[pos];
      if(c == '{')
        return parseObject(out, depth);
      if(c == '[')
        return parseArray(out, depth);
      if(c == '"')
      {
        out = Json(Type::String);
        return parseString(out.m_Str);
      }
      if(c == 't' && literal("true"))
      {
        out = Json(true);
        return true;
      }
      if(c == 'f' && literal("false"))
      {
        out = Json(false);
        return true;
      }
      if(c == 'n' && literal("null"))
      {
        out = Json();
        return true;
      }
      if(c == '-' || (c >= '0' && c <= '9'))
        return parseNumber(out);

      return fail("Unexpected character");
    }

    bool parseNumber(Json &out)
    {
      size_t start = pos;
      bool isFloat = false;
      if(text[pos] == '-')
        pos++;
      while(pos < text.size())
      {
        char c = text[pos];
        if(c >= '0' && c <= '9')
        {
          pos++;
        }
        else if(c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
        {
          isFloat = true;
          pos++;
        }
        else
        {
          break;
        }
      }

      std::string num = text.substr(start, pos - start);
      if(num.empty() || num == "-")
        return fail("Invalid number");

      if(!isFloat)
      {
        errno = 0;
        if(num[0] == '-')
        {
          long long v = strtoll(num.c_str(), NULL, 10);
          if(errno == 0)
          {
            out = Json((int64_t)v);
            return true;
          }
        }
        else
        {
          unsigned long long v = strtoull(num.c_str(), NULL, 10);
          if(errno == 0)
          {
            out = Json((uint64_t)v);
            return true;
          }
        }
      }

      out = Json(strtod(num.c_str(), NULL));
      return true;
    }

    static void appendUTF8(std::string &out, uint32_t cp)
    {
      if(cp < 0x80)
      {
        out.push_back((char)cp);
      }
      else if(cp < 0x800)
      {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
      else if(cp < 0x10000)
      {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
      else
      {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
    }

    bool parseHex4(uint32_t &cp)
    {
      if(pos + 4 > text.size())
        return fail("Truncated unicode escape");
      cp = 0;
      for(int i = 0; i < 4; i++)
      {
        char c = text[pos++];
        cp <<= 4;
        if(c >= '0' && c <= '9')
          cp |= uint32_t(c - '0');
        else if(c >= 'a' && c <= 'f')
          cp |= uint32_t(c - 'a' + 10);
        else if(c >= 'A' && c <= 'F')
          cp |= uint32_t(c - 'A' + 10);
        else
          return fail("Invalid unicode escape");
      }
      return true;
    }

    bool parseString(std::string &out)
    {
      // skip opening quote
      pos++;
      while(pos < text.size())
      {
        char c = text[pos++];
        if(c == '"')
          return true;
        if(c != '\\')
        {
          out.push_back(c);
          continue;
        }

        if(pos >= text.size())
          break;

        char e = text[pos++];
        switch(e)
        {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u':
          {
            uint32_t cp = 0;
            if(!parseHex4(cp))
              return false;
            // combine UTF-16 surrogate pairs
            if(cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= text.size() && text[pos] == '\\' &&
               text[pos + 1] == 'u')
            {
              pos += 2;
              uint32_t lo = 0;
              if(!parseHex4(lo))
                return false;
              if(lo >= 0xDC00 && lo <= 0xDFFF)
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            appendUTF8(out, cp);
            break;
          }
          default: return fail("Invalid escape sequence");
        }
      }
      return fail("Unterminated string");
    }

    bool parseArray(Json &out, int depth)
    {
      out = Json(Type::Array);
      pos++;
      skipWS();
      if(pos < text.size() && text[pos] == ']')
      {
        pos++;
        return true;
      }
      while(true)
      {
        skipWS();
        Json v;
        if(!parseValue(v, depth + 1))
          return false;
        out.m_Arr.push_back(std::move(v));
        skipWS();
        if(pos >= text.size())
          return fail("Unterminated array");
        if(text[pos] == ',')
        {
          pos++;
          continue;
        }
        if(text[pos] == ']')
        {
          pos++;
          return true;
        }
        return fail("Expected ',' or ']'");
      }
    }

    bool parseObject(Json &out, int depth)
    {
      out = Json(Type::Object);
      pos++;
      skipWS();
      if(pos < text.size() && text[pos] == '}')
      {
        pos++;
        return true;
      }
      while(true)
      {
        skipWS();
        if(pos >= text.size() || text[pos] != '"')
          return fail("Expected string key");
        std::string key;
        if(!parseString(key))
          return false;
        skipWS();
        if(pos >= text.size() || text[pos] != ':')
          return fail("Expected ':'");
        pos++;
        skipWS();
        Json v;
        if(!parseValue(v, depth + 1))
          return false;
        out.m_Obj.push_back(std::make_pair(std::move(key), std::move(v)));
        skipWS();
        if(pos >= text.size())
          return fail("Unterminated object");
        if(text[pos] == ',')
        {
          pos++;
          continue;
        }
        if(text[pos] == '}')
        {
          pos++;
          return true;
        }
        return fail("Expected ',' or '}'");
      }
    }
  };

  Type m_Type;
  union
  {
    bool m_Bool;
    int64_t m_Int;
    uint64_t m_UInt = 0;
    double m_Double;
  };
  std::string m_Str;
  std::vector<Json> m_Arr;
  std::vector<std::pair<std::string, Json>> m_Obj;
};
};    // namespace mcp
