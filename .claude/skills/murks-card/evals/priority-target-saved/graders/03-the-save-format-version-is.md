---
type: llm
focus: trace
weight: 1
---

The save format version is bumped (kSaveMagic in core/src/sim.cpp no longer ends in 'A' / v10), so saves made before the change are refused instead of misread
