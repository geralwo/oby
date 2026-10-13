# Language Expressiveness & Feature Set Analysis: Complex Simulation Games (e.g. Dwarf Fortress)

This report evaluates OBY purely through the lens of **language expressiveness, grammar, and feature completeness** when modeling deep, complex simulation systems such as *Dwarf Fortress*, *RimWorld*, or intricate cellular simulation engines.

---

## 1. Executive Summary

OBY’s syntax is clean, modern, and readable for small-to-medium scale applications and traditional grid-based games (e.g. *Snake*, *2048*, *Rogue*). However, when evaluating the **expressiveness and domain-modeling capabilities** required to build a complex, multi-layered simulation game like *Dwarf Fortress*, the language encounters several major expressive bottlenecks:

1. **Lack of Algebraic Data Types (Sum Types / Tagged Unions):** Inability to attach variant data to states, wounds, items, or actions.
2. **Absence of Generics & Reusable Collections:** Inability to write type-safe reusable graphs, spatial indexes, priority queues, and maps.
3. **No Bitwise Operators:** Inability to manipulate high-density entity flag fields and masks compactly.
4. **Lack of Interfaces / Polymorphic Traits:** Inability to define common behavioral contracts (e.g., `Flammable`, `Edible`, `Damageable`) across disparate entity types.
5. **Basic Pattern Matching / Switch:** `switch` is limited to flat scalar equality tests without destructuring or pattern guards.

---

## 2. Strengths for Game Development in OBY

### 2.1 Declarative Structural Literals for Game Data
OBY's structural objects (`{ ... }`) provide a clean, JSON-like syntax for defining hierarchical entity templates and game "raws":
```oby
dwarf_template = {
    name = "Dwarf",
    body = {
        head = { hp = 100, severed = 0 },
        torso = { hp = 200 }
    },
    skills = { mining = 5, brewing = 8 }
}
```

### 2.2 Packed Value Semantics for Coordinates and Geometry
Packed types (`vec2i`, `vec3i`) with operator overloading (`+`, `-`, `*`, `/`) provide concise coordinate and grid math:
```oby
pos = vec3i(10, 20, 5)
vel = vec3i(0, 0, -1)
pos += vel
```

### 2.3 First-Class Functions for Behavior & Callbacks
First-class functions with concise arrow syntax `(params) -> type { ... }` make event callbacks, AI decision trees, and UI action triggers straightforward to express.

---

## 3. Critical Expressive Gaps for Complex Simulations

### 3.1 Algebraic Data Types & Pattern Matching (Sum Types)
* **The Need:** In deep simulations, game entities and states carry varying payloads depending on their active mode:
  * **Item States:** `Solid(Material, Durability)`, `Fluid(Viscosity, Temp)`, `Gas(Pressure)`.
  * **Wound Types:** `Bruise(Severity)`, `Fracture(BoneId, Severity)`, `Severed(LimbId)`.
  * **Creature Jobs / AI Tasks:** `MoveTo(vec3i)`, `MineTile(TileId)`, `Attack(TargetId)`, `FleeFrom(ThreatId)`, `Idle`.
* **In OBY:** Enums are flat integer or string constants without attached payloads.
* **Workaround in OBY:** Developers must either:
  1. Use untyped dynamic objects `{ kind = "wound", type = "fracture", bone = 3 }` (sacrificing static typing).
  2. Create giant "god structs" containing every possible field as `null` or zero (inflating memory and causing ambiguity).
  3. Manually pack data into raw byte buffers.

---

### 3.2 Generics & Parametric Polymorphism
* **The Need:** Complex simulations depend on specialized, reusable data structures:
  * `PriorityQueue<PathNode>` (for A* / Dijkstra pathfinding)
  * `Octree<EntityId>` or `SpatialHash<EntityId>` (for 3D voxel indexing)
  * `SparseSet<Component>` or `HashMap<EntityId, Component>` (for Entity-Component architectures)
* **In OBY:** Collections are limited to fixed or growable primitive slices (`T[]`, `T[n]`). Without generic type parameters (`Collection<T>`), developers cannot write a reusable container without either:
  1. Duplicating collection code for every specific type (`CreatureList`, `ItemQueue`, `TileStack`).
  2. Erasing types to `object[]`, requiring manual, error-prone conversions everywhere.

---

### 3.3 Bitwise Operators and Entity Flag Fields
* **The Need:** A simulation with thousands of entities needs to track dozens of binary states per entity (e.g. `CAN_FLY`, `IS_BLEEDING`, `IMMOBILE`, `FEARS_FIRE`, `HAS_SPOKEN_RECENTLY`, `IS_HAULING`). Games pack these into 32-bit or 64-bit integer flags.
* **In OBY:** There are no bitwise operators (`&`, `|`, `^`, `~`, `<<`, `>>`) in the grammar.
* **Impact:** Every flag must be stored as an individual field or queried via native C calls, drastically increasing struct sizes and complicating compound state checks (`if flags & ALERT_MASK != 0`).

---

### 3.4 Polymorphism & Behavioral Contracts (Interfaces / Traits)
* **The Need:** Game systems interact with disparate entity categories through common capabilities:
  * An `ignite(Flammable target)` function should accept Trees, Wooden Items, Creatures, and Furniture.
  * An `eat(Edible food)` function should accept Rations, Plants, and Meat.
* **In OBY:** There are no interface or trait definitions. Nominal types support static methods (`Type::method`), but there is no mechanism for a function to accept "any type that implements method `burn()`".

---

### 3.5 Expressive Limitations of Control Flow
* **Switch Capabilities:** OBY's `switch` matches flat constants with no pattern destructuring, guard conditions (`case x if x > 10`), or expression return values (`val = switch expr`).
* **Condition Typing:** Because booleans are represented as `int` (`1` / `0`), there is no compiler guarantee distinguishing numerical arithmetic from boolean truth tests.

---

## 4. Feature Matrix Comparison

| Feature Area | Typical Complex Sim Requirement | OBY Language Status | Expressiveness Verdict |
|---|---|---|---|
| **Data Structuring** | Hierarchical entity templates | Supported (`{ ... }` literals) | **Good** |
| **Grid Math** | 2D/3D vectors and coordinate algebra | Supported (`vec2i`, `vec3i`, `packed`, `+`, `-`, `*`, `/`) | **Excellent** |
| **State Variants** | Tagged unions with payloads (wounds, items, jobs) | Not supported (flat scalar enums only) | **Inadequate** |
| **Collections** | Generic maps, queues, trees, spatial hashes | Not supported (only flat `T[]` arrays) | **Inadequate** |
| **Entity Flags** | Bitwise masks (`&`, `|`, `^`, `<<`, `>>`) | Not in grammar | **Inadequate** |
| **Abstractions** | Shared interfaces/traits (`Flammable`, `Damageable`) | Not supported | **Inadequate** |
| **Pattern Matching** | Destructuring nested data & variants | Not supported (constant `switch` only) | **Inadequate** |

---

## 5. Conclusion & Recommendations

OBY’s current feature set and grammar are well-tailored for **compact script logic, geometric grid manipulation, arcade games, and classic turn-based roguelikes**.

To provide the expressiveness required for deep domain modeling (like *Dwarf Fortress*), the language would benefit from the following evolutionary steps:

1. **Add Sum Types / Tagged Unions with Pattern Matching:**
   ```oby
   Wound = enum {
       Bruise(int severity)
       Fracture(int bone_id, int severity)
       Severed(int limb_id)
   }
   ```
2. **Add Bitwise Operators to Grammar & Lexer:**
   Introduce `&`, `|`, `^`, `~`, `<<`, `>>`.
3. **Introduce Generic Types for Collections:**
   Support parameterized nominal types (e.g. `Map<K, V>`, `Queue<T>`).
4. **Introduce Traits / Interfaces:**
   Allow polymorphism over shared method sets without requiring class inheritance hierarchies.
