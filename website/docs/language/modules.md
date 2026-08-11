# Modules

Each `.be` or `.bee` file is a module. The `.be`/`.bee` extension is added
automatically, and dotted names map to sub-directories (`a.b.c` → `a/b/c.bee`).

`import name` looks, in order, in:

1. the importing file's own directory
2. its sibling `lib/` folder
3. `hive_modules/` in that directory and every directory above it - packages
   installed by [Hive](../hive.md), the package manager
4. each entry of `$BEE_PATH` (`;`-separated on Windows, `:` elsewhere)
5. the global package library, `$HIVE_HOME/lib` (default `~/.hive/lib`)

Local code therefore wins over an installed package of the same name. A
directory found this way is treated as a package: the module loaded is the
`"main"` named by its `hive.json`, or else `init.bee`.

`mathutil.bee`:
```
let PI = 3.14159265358979

fn square(x) { return x * x }
```

Importing it:

```
import mathutil                      # bind the module object as `mathutil`
print(mathutil.square(5))            # 25
print(mathutil.PI)

import mathutil as m                 # bind under an alias
print(m.square(6))

from mathutil import square          # bring specific names into scope
print(square(7))

from mathutil import square as sq    # ... with an alias
print(sq(8))

from mathutil import *               # bring in all public names
print(square(9))
```

`from … import *` skips names beginning with `_` (treat those as private).
Circular imports are tolerated (a module is cached as soon as it starts loading).

To use somebody else's module, install it with `hive install <name>` and import
it by name - see the [Hive guide](../hive.md).

A module can also be **native**: a shared library written in C++ (or generated
from a C++ header by `beegen`) imports exactly like a `.bee` file. See the
[bindings guide](../bindings.md).
