// Grammar-loader fixture for the registry doors (post-1.8.0
// grammar-extensibility sliver, PLAN) - NOT a ctest entry; tst_codeeditor
// loads it by path (QMN_TEST_FAKE_GRAMMAR). It honors tree-sitter's
// loader CONTRACT (a no-arg exported function named tree_sitter_<base>)
// but returns a ZEROED, TSLanguage-SIZED buffer instead of a real
// language: language version 0 is outside the runtime's accepted range
// (13-15 for v0.27.2), so the runtime refuses it during the scratch-
// parser validation registerGrammar runs at the door. The test pins
// that a plausible-but-bogus library is rejected with `false` - no
// crash, no half-registered id, and the library itself loads fine
// (loadGrammar's own success channel is separate from validation).
//
// Deliberately includes NO tree-sitter header: the payload is opaque
// shape-only (the runtime reads version at the struct head, finds 0,
// and returns before touching any deeper field).
static unsigned long long g_zeroed_language[512]; // 4 KiB of nothing

void* tree_sitter_bogus(void)
{
    return g_zeroed_language;
}
