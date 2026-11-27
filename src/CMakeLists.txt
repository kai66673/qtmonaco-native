add_subdirectory(core)
add_subdirectory(diff)
# The search engine (GATE 4 car I1): the QtCore-only sibling of core/diff
# (headless walk + in-buffer scan; the IDE panel is its first consumer).
add_subdirectory(search)
add_subdirectory(protocol)
add_subdirectory(lsp)
# The DAP layer (car 4, machine D2): sibling of lsp over the protocol
# mechanism (EnvelopeCodec seam), never linked to it, never view/core.
add_subdirectory(dap)
add_subdirectory(view)
# The glue shelf (post-M8 train, PLAN: the glue shelf + ProjectSession):
# a REAL layer (client of core+view+protocol+lsp), its own export set —
# the old M8 "src/app/glue is the ONE app target that knows lsp/protocol"
# clause is retired: app targets now merely LINK the qmn_glue target.
add_subdirectory(glue)
add_subdirectory(app)
