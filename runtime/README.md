# Runtime

The platform layer shared by every game in this repository. It is empty until
milestone 1 (`docs/ROADMAP.md`): the first files will be the memory map, the
BIOS functions and the stubs that let the game link.

Rule: nothing in here may know which game it is serving. Game-specific hooks go
in `games/<game>/port/`.
