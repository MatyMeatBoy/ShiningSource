# Shining Soul 1: closed

There is no decompilation of *Shining Soul* (GBA) yet, so this game cannot be
built. This folder is a placeholder.

When the decompilation exists:

1. add it as the submodule `games/ss1/decomp`;
2. register it in `tools/build.py` (`GAMES`);
3. run `python3 tools/build.py fix --game ss1` and work through
   [the roadmap](../../docs/ROADMAP.md) from milestone 0.

The two games share an engine family, so the runtime written for Shining Soul II
is expected to carry over. The method for the decompilation itself is in
SS2Decomp (`docs/agents/GIANT_FUNCTION_PLAYBOOK.md`, `DATA_PIPELINE.md`).
