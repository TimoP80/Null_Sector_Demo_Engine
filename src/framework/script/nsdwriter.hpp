// ---------------------------------------------------------------------------
// NsdWriter - the writer half of the demo DSL: serializes a parsed Script AST
// back to .nsd source text, the inverse of ScriptParser::parse. Used by the
// editor document layer (save / undo / add-scene write the production) and
// unit-tested for exact structural round-trips.
//
// Round-trip contract: parse(nsdSerialize(parse(x))) is structurally equal to
// parse(x). Comments, blank lines and original formatting are not preserved.
// ---------------------------------------------------------------------------
#pragma once

#include "framework/script/scriptparser.hpp"
#include <string>

namespace ns {

/** serialize a parsed script back to .nsd source text */
std::string nsdSerialize(const Script& s);

/** serialize a single command (used by the editor for on-the-fly preview and
 *  diagnostics; matches the command rendering inside nsdSerialize) */
std::string nsdSerializeCmd(const Cmd& c);

}  // namespace ns
