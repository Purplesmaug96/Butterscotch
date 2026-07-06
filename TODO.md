    # TODO (BlackboxAI)

## Phase 1: D3D9 renderer scan + parity fixes
- [ ] Identify and fix shader uniform/register handling issues in `src/d3d9/d3d9_renderer.cpp` (focus on correctness)
- [ ] Reduce expensive D3D9 draw-call patterns (especially non-X360 paths, but keep changes safe for X360)
- [ ] Remove redundant flush/set-state logic where it can be avoided without behavior changes
- [ ] Refactor duplicated texture-page validation / ensure-load / UV calculation into helpers

## Phase 2: Xbox 360 focus verification
- [ ] Ensure all refactors keep Xbox 360 behavior intact (viewport/viewportenable, shader toggling, texture upload/eviction)
- [ ] Validate async texture loading state transitions remain correct

## Phase 3: Cleanup
- [ ] Remove dead code/unused paths and improve readability/comments
- [ ] Ensure compilation is still expected to succeed (no build run by instruction)

