#pragma once
//
// hp/features.hpp — compile-time ablation flags.
//
// Each ported model / mixer change is a switch so A.3 can accept or reject
// one axis at a time. Defaults are the high-confidence set from PLAN.md.
// Override with -DHP_WIKI_STATES=0 etc.
//
// NOTHING here is transmitted. Encoder and decoder are the same binary, so
// they compile the same set. Changing a flag produces a different archive
// family (kVersion bumped when the default set changes).


#include "hp/config.hpp"
