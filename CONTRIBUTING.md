# VMHook Coding Style Guide

Comprehensive style guide for C++ and Java code in the VMHook project.
All code must adhere to these standards.

---

## Table of Contents

- [C++ Style](#c-style)
  - [General Principles](#general-principles)
  - [Naming Conventions](#naming-conventions)
  - [Function Syntax](#function-syntax)
  - [Namespace Usage](#namespace-usage)
  - [Control Structures & Formatting](#control-structures--formatting)
  - [Header-Only Library Documentation](#header-only-library-documentation)
  - [Low-Level & Systems Programming](#low-level--systems-programming)
- [Java Style](#java-style)
  - [File Encoding & Line Endings](#file-encoding--line-endings)
  - [Indentation](#indentation)
  - [Naming](#naming)
  - [Method Signatures](#method-signatures)
  - [Types](#types)
  - [Control Flow](#control-flow)
  - [Field Declarations](#field-declarations)
  - [Comments](#comments)
  - [Strings and Text](#strings-and-text)
  - [Error Handling](#error-handling)
  - [Imports](#imports)
  - [Miscellaneous](#miscellaneous)
- [Project-Specific Conventions](#project-specific-conventions)
  - [C++ Header-Only Library](#c-header-only-library)
  - [Java Test Target](#java-test-target)

---

## C++ Style

### General Principles

- **Language**: Always write code and comments in English.
- **Clarity over Brevity**: Prefer verbose, descriptive code over clever or short snippets.
- **No Abbreviations**: Never shorten names (e.g., use `pointer` instead of `ptr`, `index` instead of `idx`).
- **Standard**: Use modern C++ features up to **C++23** (excluding modules).

### Naming Conventions

- **Snake Case**: Use `snake_case` for variables, functions, classes, and templates.

```cpp
auto compute_value() -> std::int32_t;

template<typename value_type>
struct container_type;
```

- **Type Suffixes**: Always use the `_t` suffix for type aliases and typedefs.

```cpp
using integer_value_t = std::int32_t;
using container_t = std::vector<std::int32_t>;
```

- **Constants**: Use `lower_snake_case` for constants.

```cpp
constexpr auto default_capacity = 64;
constinit auto global_instance_count = 0;
```

- **Compile-Time and Initialization Specifiers**: Always use `constinit`, `constexpr`, and `consteval` when they are applicable.

```cpp
constexpr auto buffer_size = 4096;

consteval auto compute_alignment(std::size_t value) -> std::size_t
{
    return value * 2;
}

constinit auto runtime_initialized_counter = 0;
```

- **Exception Specifications**: Use `noexcept` whenever a function can guarantee it will not throw.

```cpp
auto reset_state() noexcept -> void;
```

- **Template Parameter Names**: Never use abbreviated template parameter names such as `T` or `C`. Use descriptive `snake_case` names.

```cpp
// Good
template<typename derived_type, class parent_class>
struct relationship_t;

// Bad
template<typename T, class C>
struct relationship_t;
```

### Function Syntax

- **Trailing Return Formatting**: Place the return type on a new line, indented with a tab.

```cpp
auto get_field_bool()
    -> bool
{
}
```


- **Trailing Return Types**: Always use the trailing return type syntax.
- **Explicit Constructors**: Use the `explicit` keyword only for constructors with exactly one parameter.
- **Brace Placement**: Never place the opening brace `{` on the same line as the function signature.

- **One Action Per Line**: Never put multiple statements on a single line (no `else { ... }` on one line, no `if (x) { y; }` on one line).

```cpp
// Good
if (condition)
{
    do_something();
}
else
{
    do_something_else();
}

// Bad
if (condition) { do_something(); }
else { do_something_else(); }
```

- **Brace Initialization**: Prefer brace initialization `{}` with a space inside the braces.

```cpp
std::int32_t value{ 0 };  // Good
std::int32_t value{0};    // Bad - missing space
std::int32_t value(0);    // Bad - parentheses
```

### Namespace Usage

- **Full Qualification**: Always use the full namespace path, even when calling a function or using a type within the same namespace.

```cpp
namespace application::math
{
    auto add(std::int32_t a, std::int32_t b) -> std::int32_t
    {
        return a + b;
    }

    auto calculate() -> void
    {
        // Good
        auto result = application::math::add(1, 2);

        // Bad
        auto result = add(1, 2);
    }
}
```


- **Member Access**: Always use `this->` when accessing member variables or functions.

```cpp
this->value = 10;
auto result = this->compute_value();
```

- **Private Fields Naming**: Do not use prefixes or suffixes such as `m_` or `_`. Use standard variable naming.

```cpp
class example
{
private:
    std::int32_t value; // Good

    // Bad examples:
    // std::int32_t m_value;
    // std::int32_t value_;
};
```

### Control Structures & Formatting

- **Explicit Scopes**: Always use braces `{}` for control structures (`if`, `else`, `for`, `while`), even for single statements.
- **One Action Per Line**: Never put multiple statements or actions on a single line.

```cpp
// Good
if (condition)
{
    do_something();
}

// Bad
if (condition) { do_something(); }
```

- **Initialization**: Prefer brace initialization `{}`.

```cpp
std::int32_t value{1};  // Good
```

- **IO**: Prefer `<print>` (C++23) over `<iostream>`.

```cpp
std::print("Value: {}\n", value);  // Good
std::cout << "Value: " << value << std::endl;  // Bad
```

### Header-Only Library Documentation

When developing C++ header-only libraries, provide **complete and exhaustive documentation** within the file. This includes:

1. **Top-level Overview**: Purpose, dependencies, and thread-safety guarantees.
2. **Template Requirements**: Explicitly document requirements for template parameters (using Concepts where applicable).
3. **Complexity Guarantees**: Big-O notation for time and space complexity for every public method.
4. **Exception Safety**: Specify if a function is `noexcept` or what exceptions it might throw.

```cpp
/*
    @brief Brief description of the function.
    @details
    Detailed explanation of what this function does, including any
    important implementation details or constraints.

    Complexity: O(n) time, O(1) space
    Exception safety: noexcept
    Thread safety: not thread-safe

    @param param1 Description of param1
    @param param2 Description of param2
    @return Description of return value
*/
auto example_function(std::int32_t param1, const std::string& param2) -> bool
{
}
```

### Low-Level & Systems Programming

When performing low-level operations (memory mapping, hardware abstraction, drivers), you must be explicit:

- **Address Manipulation**: Explain the rationale behind every pointer arithmetic operation.
- **Offsets**: Document why a specific offset is used (e.g., referencing a specific struct field).
- **Bit Manipulation**: For every bitwise AND, OR, XOR, or shift, provide a comment explaining which bit-fields are being affected and why.

```cpp
// Masking bits 4-7 to extract the interrupt priority level
// Offset 0x04 corresponds to the Interrupt Control Register (ICR)
auto priority = (*(interrupt_register + 0x04) >> 4) & 0x0F;
```

---

## Java Style

### File Encoding & Line Endings

- UTF-8 encoding, Unix-style line endings (LF).
- No BOM.
- **File naming**: `PascalCase.java` (standard Java convention, e.g., `TestTarget.java`, `Main.java`).

### Indentation

- Opening brace `{` on the same line as the declaration.
- Closing brace `}` on its own line, at the same indentation as the opening keyword.

```java
public class TestTarget {
    private int value;

    public TestTarget(int v) {
        this.value = v;
    }
}
```

### Naming

- **Classes**: `PascalCase`  `TestTarget`, `Main`, `HttpClient`.
- **Methods**: `camelCase` with first letter lowercase  `getField`, `runSelfTest`, `onTick`.
- **Fields / variables**: `camelCase`  `staticInstance`, `passed`, `sb`.
- **Constants** (`static final`): `lower_snake_case`  `max_retry`, `default_port`.
- **Type parameters**: single uppercase letter  `T`, `U`.

### Method Signatures

- Use standard Java syntax for the actual signature.

```java
public boolean getFieldBool() {
    return getField("fieldBool").get();
}

public void setFieldBool(boolean value) {
    getField("fieldBool").set(value);
}
```

### Types

- Always use **explicit typing**  no `var`, no diamond operator inference when avoidable.
- Prefer primitive types over boxed types when the value is never `null`.
- For collections, declare against the interface with explicit type parameters:

```java
List<String> list = new ArrayList<String>();
```


- **Member Access**: Always use `this.` when accessing instance fields.

```java
this.value = 10;
boolean state = this.fieldBool;
```

### Control Flow

- Braces `{}` are **required** even for single-statement blocks.
- Braces open on the same line as the keyword (`if`, `for`, `while`).

```java
if (ok) {
    doSomething();
}
```

- `switch` statements must include a `default` case (even if empty).
- No magic numbers  use named constants.

### Field Declarations

- One field per declaration.
- Fields at the top of the class, grouped by visibility:
  1. `public static final` constants
  2. `public static` variables
  3. `public` instance variables
  4. `private` / `protected` variables

```java
public class TestTarget {
    // Static primitives
    public static boolean staticBool = true;
    public static byte    staticByte = (byte) -127;

    // Instance primitives
    public boolean fieldBool = true;
    public byte    fieldByte = 127;
}
```

### Comments

- Use `//` for short inline comments.
- Use `/** ... */` Javadoc only for public API classes and methods.
- Avoid commented-out code in committed files.

```java
// Hook counter
public static volatile int hookCallCount = 0;

// Decode the compressed OOP before use
void* oop = decodeOopPointer(compressed);
```

### Strings and Text

- Use `String` (not `StringBuilder`) for simple assignments.
- Prefer `StringBuilder` for concatenation in loops.
- String literals use double quotes: `"hello"`, not single quotes.

### Error Handling

- Never silently swallow exceptions. At minimum, log or rethrow.
- Use try-with-resources for `AutoCloseable` resources.

### Imports

- Do not use wildcard imports (`import java.util.*;`).
- Organize imports in this order:
  1. `java.lang` (implicit, do not write)
  2. `java.util`, `java.io`, etc.
  3. Third-party libraries
  4. Project packages (`vmhook.example.*`)

### Miscellaneous

- Always use `@Override` when implementing interface or overriding superclass methods.
- Override `toString()`, `hashCode()`, and `equals()` when appropriate.
- Array literals: `new int[]{ 1, 2, 3 }` not `new int[3]{ 1, 2, 3 }`.

---

## Project-Specific Conventions

### C++ Header-Only Library

> **Edit `vmhook.ixx`, never `vmhook.hpp`.** The module is the library; the header
> is derived from it by `tools/make_header.py`, which strips every C++26 construct
> and substitutes a C++23 answer. An edit made in the header is lost at the next
> regeneration, and the two files silently disagreeing is the failure mode the
> generator exists to prevent. Run `python tools/make_header.py` after any change
> to the module, and `--check` to confirm the committed header is current.
>
> **`vmhook.hpp` is C++23 and stays C++23.** If a change to the module needs a
> C++26 feature, the generator needs a C++23 substitute for it in the same commit.

For `vmhook.ixx` / `vmhook.hpp` and related C++ files:

- **Single-file library**: All code in one module interface unit, with no external dependencies beyond the standard library and Win32.
- **Trailing return types**: Always use `auto function_name() -> return_type`.
- **Explicit constructors**: Use `explicit` only for constructors with exactly one parameter.
- **Brace initialization**: `int x{0};` not `int x(0);` or `int x = 0;`.
- **No non-ASCII characters**: Do not use Unicode box-drawing characters or em-dashes in C++ code or comments. Use ASCII alternatives like `---` or `===`.
- **vmhook::object subclass pattern**:

```cpp
class test_target : public vmhook::object
{
public:
    explicit test_target(vmhook::oop_t instance)
        : vmhook::object{ instance }
    {
    }

    auto get_field_bool()
	-> bool
    {
        return get_field("fieldBool")->get();
    }
};
```

### Java Test Target

For `TestTarget.java` and related Java files:

- **camelCase field names**: Java fields use `camelCase` (e.g., `fieldBool`, `staticInt`).
- **camelCase method names**: Java methods use `camelCase` (e.g., `getFieldBool`, `runSelfTest`).
- **PascalCase class names**: Java classes use `PascalCase` (e.g., `TestTarget`, `Main`).
- **No non-ASCII characters**: Do not use Unicode box-drawing characters or em-dashes. JDK 8 on Windows uses Cp1252 encoding and will fail to compile.
- **Field name alignment**: C++ `get_field("fieldName")` calls must exactly match Java field names.

```java
public class TestTarget {
    public boolean fieldBool = true;
    public static int hookCallCount = 0;

    public void onTick(int tick) {
        this.fieldInt = tick % 10000;
    }

    public static void runSelfTest() {
        // Self-test implementation
    }
}
```

---

## Quick Reference

### C++ Checklist

- [ ] Trailing return types (`auto func() -> type`)
- [ ] Explicit constructors (`explicit class_name(args)`)
- [ ] Brace initialization (`int x{0};`)
- [ ] Snake case naming (`function_name`, `variable_name`)
- [ ] Type suffix for aliases (`using my_type_t = ...;`)
- [ ] Constants in lower snake case (`max_retry`)
- [ ] Use `constinit`, `constexpr`, and `consteval` when applicable
- [ ] Use `noexcept` when applicable
- [ ] Descriptive template names (`derived_type`, `parent_class`)
- [ ] No abbreviations
- [ ] Full namespace qualification
- [ ] Braces on new line for functions/classes
- [ ] Braces on same line for control structures
- [ ] No non-ASCII characters
- [ ] Comments in English

### Java Checklist

- [ ] PascalCase class names (`TestTarget`)
- [ ] camelCase method names (`getFieldBool`)
- [ ] camelCase field/variable names (`fieldBool`, `passed`)
- [ ] lower_snake_case constants (`max_retry`)
- [ ] Braces on same line as declaration
- [ ] Braces required for all control structures
- [ ] No non-ASCII characters
- [ ] Comments in English
- [ ] Explicit typing (no `var`)
- [ ] No wildcard imports