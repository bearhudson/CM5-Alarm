I had an idea to use two MAX7219s as a Home Assistant display panel but I thought the LEDs were RGB LEDs not single color, so since I ordered too many, I decided to make two into a CM-5 emulator / alarm clock.

Gemini and I came up with these four types of workloads being emulated to show realistically what the actual LED nodes would look like during major processing jobs. 

Workload 1: 2D Grid Stencil / Boundary Exchange (Fluid/PDE Solvers): Nodes read their own memory and shift boundary values to their 4 orthogonal neighbors ($N, S, E, W$).

Workload 2: Systolic Array Matrix Multiplication: Data streams continuously through the memory registers horizontally and vertically, pulsing memory buses on every multiply-accumulate shift.

Workload 3: Fat-Tree Global Reduction / Barrier Sync: Hierarchical binary/quad tree routing where leaf nodes forward partial sums up the tree to the master partitions, pulsing in branching harmonic clusters.

Workload 4: Vector Sweep / Memory Bus Sieve: Linear continuous memory traversals across nodes.
