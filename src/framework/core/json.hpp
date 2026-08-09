// ---------------------------------------------------------------------------
// Json - a small self-contained JSON parser/serializer over Value.
// No third-party dependency; errors throw JsonError with line/column context.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/core/value.hpp"
#include <stdexcept>
#include <string>

namespace ns {

class JsonError : public std::runtime_error {
public:
  explicit JsonError(const std::string& msg) : std::runtime_error(msg) {}
};

namespace Json {

/** parse a JSON document (object/array/scalar); throws JsonError on failure */
Value parse(const std::string& text);

/** read a JSON file from disk; throws JsonError (wrap for file errors) */
Value parseFile(const std::string& path);

/** serialize a Value to JSON text (pretty when indent >= 0) */
std::string serialize(const Value& v, int indent = 2);

/** serialize and write a Value to a file */
void writeFile(const std::string& path, const Value& v, int indent = 2);

}  // namespace Json

}  // namespace ns
