# GitHub Copilot Instructions for ESP32/Arduino Projects

## Project Overview
This is a PlatformIO/Arduino project for an ESP32-based device with embedded systems development.

## Code Style Guidelines

### Naming Conventions
- **Methods**: Use camelCase
- **Member variables**: Prefix with underscore in camelCase
- **Local variables**: Use camelCase
- **Constants**: Use SCREAMING_SNAKE_CASE (all caps with underscores)
- **Types**: Use PascalCase
- Use Ptr suffix for fields and variables which are pointers
- Use nullptr instead of NULL

### Layout
- Use Allman style for curly braces
- Use proper formatting with 4-space indentation
- Keep fields and methods together. Fields first.

### Type Usage
- Use `float` instead of `double` for performance on embedded systems
- Use specific integer types like `uint32_t`, `uint16_t`, `uint8_t`, but also `int` for general integers
- Use `size_t` for array/buffer sizes
- Prefer explicit types over `auto`, unless the type name is very complex

## Development Practices
- Use Object-Oriented practices:
  - Keep data and logic together, put the logic as close to the data as possible
  - Minimize visibility of class members.
  - Fields should always be private; use get/set accessor methods.
  - Consider using class inheritance and polymorphism (but mind the memory usage)
- Minimize memory usage, but not at the expense of readability
- Try to use const-correctness for all methods
- Prefer in-class field initializers over constructor initialization lists
- Inline simple getter/setter accessor methods in header files
- Use clear, semantic names for variables and fields
- Maintain backward compatibility of public APIs
- Add comments for complex stuff, but don't state the obvious.

## Testing
- After making changes, build the project with PlatformIO Test environment.
- Ensure there are no compiler errors.
- Try to prevent compiler warnings.

## File Organization

```
include/
  *.h                 - Header files with class and interface definitions

src/
  main.cpp            - Main application entry point
  *.cpp               - Implementation files

data/
  *                   - Data files (configuration, assets, etc.)
```

## Dependencies

### Internal Libraries
- Custom libraries in `../Libraries/custom/` (Tracer, TimeUtils, etc.)

### External Libraries
- ESP32 core libraries (WiFi, FS, etc.)
- Arduino compatible APIs (Print, String, etc.)

