# QuasiLang Syntax Guide

QuasiLang is the source language parsed by **QuasiPiler**.
Sample programs: `test00.qc`–`test12.qc`.

---

## Basics

* **Comments**
    * Line: `// comment`
    * Block: `/* comment */` — may be inline or multi-line.
* **Whitespace**: ignored except as a separator.
* **Identifiers**: letters, digits, `_` (cannot start with a digit).
* **Literals**
    * Numbers: integers, floats, exponents.
    * Strings: `'...'` or `"..."` with escapes (`\n`, `\t`, `\\`).
    * Booleans: `true`, `false`
    * Null: `null`

---

## Expressions

* **Arithmetic & Assignment**: `+ - * / %` with `= += -= *= /= %=`
* **Comparison & Logic**: `== != < <= > >=` and `&& || !`
* **Bitwise & Shift**: `& | ^ << >>` with `&= |= ^= <<= >>=`
* **Increment/Decrement**: prefix and postfix `++ --`
* **Ternary**: `cond ? a : b`
* **Member access**: `obj.key`, `obj["key"]`
* **Indexing & slicing**:
    * Single index: `arr[i]`
    * Slices: `arr[start:end:step]`
    * Any part may be omitted: `arr[:end]`, `arr[start:]`, `arr[::step]`, `arr[::]`
* **Function calls**: `f(arg1, arg2)`
* **Function declaration**: `fu(x,y){ return x+y; }`

---

## Data Structures

* **Lists**: `[1, 2, 3]`
* **Objects**: `{ "key": value, "other": 42 }`
  *Keys must be constant strings.*
* **Tuples `()`**

    * Foreach bundles (abstract streams)
    * Not concrete lists unless materialized with `list(...)`

### Tuple-based foreach

Tuples drive vectorized foreach expansion when applied after an expression:

1. **Member/Index tuple**

   ```qc
   obj.(a, b, c)       // (obj.a, obj.b, obj.c)
   arr[(i, j, k)]      // (arr[i], arr[j], arr[k])
   ```

2. **Operators with tuples**

   ```qc
   5 + (1,2,3)         // (6, 7, 8)
   list(5 + (1,2,3))   // [6, 7, 8]
   ```

3. **Chaining**

   ```qc
   obj.(a,b).(x,y)       // (obj.a.x, obj.a.y, obj.b.x, obj.b.y)
   arr[(1,4)].(id,total) // (arr[1].id, arr[1].total, arr[4].id, arr[4].total)
   ```

4. **Slices with tuples**

   ```qc
   arr[1:6:2][(2,3)]   // (arr[1][2], arr[1][3], arr[3][2], arr[3][3], arr[5][2], arr[5][3])
   list(arr[1:6:2])[2] // arr[5]
   ```

---

## Statements & Declarations

* **Variables**: `name = expression;`
* **Functions**: `name(p1,p2){ ... }` or `name = fu(p1){ ... };`
* **Conditionals**:

  ```qc
  if (cond) { ... }
  elif (other) { ... }
  else { ... }
  ```

  Bodies may omit braces for a single statement:

  ```qc
  if (cond) do_something();
  while (ok) step();
  ```

  Return with conditional:

  ```qc
  return if(a){b} else c;   // valid
  return if(a) b; else c;   // invalid due to precedence
  ```
* **Loops**: `while(cond){...}`, `for(init; cond; step){...}`
  Both also allow single-statement bodies without `{}`.
* **Exceptions**:

  ```qc
  try { ... } catch (err) { ... } finally { ... }
  ```
* **Jumps**: `break;`, `continue;`, `return expr;`, `goto label;`
* **Labels**: `label_name:`

---

## Examples

**Foreach with tuples**

```qc
user = { "id": 7, "name": "Ada", "meta": { "city":"Paris", "tz":"CET" } };

user.(id, name)                 // 7, "Ada"
user.(meta).("city","tz")       // "Paris", "CET"
orders[(0,2)].(id,total)        // orders[0].id, orders[0].total, orders[2].id, orders[2].total
```

**Slices and foreach**

```qc
matrix = [row0,row1,row2,row3,row4,row5];
matrix[1:6:2][(2,3)];           // row1[2], row1[3], row3[2], row3[3], row5[2], row5[3]
list(matrix[1:6:2])[2];         // row5
```

**Arithmetic with tuples**

```qc
print(5 + (1,2,3));             // 6,7,8
print(list(5 + (1,2,3)));       // [6,7,8]
```

**Control flow**

```qc
if (ready) start();
while (i < n) i++;
return if(ok){result} else null;
```

**Chained functions**

```qc
adder = fu(x){ return fu(y){ return fu(z){ return x+y+z; }; }; };
sum = adder(1)(2)(3);
```
