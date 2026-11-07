#pragma once

// Test-only hit logging, built with `--trace`. Never part of a normal build.

#include <cstddef>

class CEntityInstance;
class CSchemaSystemTypeScope;

namespace trace
{

bool Init(CSchemaSystemTypeScope *scope, char *error, std::size_t error_size);
void Frame(int slot, CEntityInstance *pawn);

} // namespace trace
