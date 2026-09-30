# Domain documents setup

Use a single context by default: `CONTEXT.md` and `docs/adr/` at the repository root. Offer a multi-context layout only when exploration finds genuine monorepo signals; then use `CONTEXT-MAP.md` to point to each context's `CONTEXT.md` and ADR directory.

Record the selected layout in `docs/agents/domain.md`. The document should tell engineering skills to read the root map or relevant context and ADRs before exploring, proceed silently when optional files do not exist, use glossary vocabulary, and surface conflicts with existing ADRs.
