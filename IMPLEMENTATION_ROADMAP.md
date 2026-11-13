# JSON Schema Validator — Implementation Roadmap

## Quick Start

### Current State
Foundation tasks (0-2) are **COMPLETE** and testable:
- ✅ NIF framework with dual validation modes (fail-fast/collect-all)
- ✅ Bytecode compiler for 13 core keywords
- ✅ Validation engine with 14 constraint checks
- ✅ Error path tracking and timeout support

### Building & Testing

```bash
# Compile the NIF
cd /home/s.aleynikov/projects/erl-libs/glazer
make -C c_src

# Run foundation tests
mix test test/glazer_json_schema_test.erl

# Expected: ~30 passing tests
```

### Basic Usage Example

```erlang
% Define a schema
Schema = #{
  type => <<"object">>,
  properties => #{
    name => #{type => <<"string">>, minLength => 1},
    age => #{type => <<"integer">>, minimum => 0, maximum => 150}
  },
  required => [<<"name">>, <<"age">>]
},

% Compile once, reuse many times
{ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),

% Validate in fail-fast mode (default)
ok = glazer_json:validate(CompiledSchema, #{
  name => <<"Alice">>,
  age => 30
}),

% Validate with error collection
{error, Errors} = glazer_json:validate(
  CompiledSchema,
  #{name => 42, age => 200},
  [collect_all_errors]
),

% Inline validation (compile + validate)
ok = glazer_json:validate_term(Schema, #{name => <<"Bob">>, age => 25}).
```

---

## Task Breakdown & Implementation Order

### Phase 1: Foundation (COMPLETE) ✅

- [x] Task 0: NIF Framework
- [x] Task 1: Bytecode Compiler
- [x] Task 2: Validation Engine

**Status**: Ready for next phase. All 14 basic constraint checks working.

---

### Phase 2: Type & Constraint Tasks (Next — 1.5-2 weeks)

**Parallelizable**: These can be worked on independently.

#### Task 3: Type Checking (`type`) — 1 day
**Status**: Already implemented in Task 2
**What's left**: Edge cases and tests
- [x] Basic type checking (bitmask)
- [ ] Test all type combinations
- [ ] Test invalid types in schema
- [ ] Test "number" type (float OR integer)

**Files to modify**:
- Add tests to `test/glazer_json_schema_test.erl`

**Estimated effort**: 1 day

---

#### Task 4: Numeric Constraints — 1 day
**Status**: Already implemented in Task 2
**What's left**: Refinement and tests
- [x] `minimum`, `maximum`
- [x] `exclusiveMinimum`, `exclusiveMaximum`
- [x] `multipleOf`
- [ ] Test with floats vs. integers
- [ ] Test bigints
- [ ] Test edge cases (0, negative, max int64)

**Files to modify**:
- `test/glazer_json_schema_test.erl` — add test cases

**Test vectors**:
```erlang
% minimum: 0, maximum: 150
#{age => -1}   => error
#{age => 0}    => ok
#{age => 150}  => ok
#{age => 151}  => error

% multipleOf: 5
#{count => 10} => ok
#{count => 11} => error
```

**Estimated effort**: 1 day

---

#### Task 5: String Constraints — 1.5 days
**Status**: Already implemented in Task 2
**What's left**: UTF-8 edge cases and regex patterns
- [x] `minLength`, `maxLength` (UTF-8 grapheme counting)
- [x] `pattern` (regex matching via pre-compiled patterns)
- [ ] Test UTF-8 multi-byte chars: "😀" = 1 grapheme, 4 bytes
- [ ] Test regex edge cases (empty, special chars, escapes)
- [ ] Test pattern errors in schema

**Files to modify**:
- `test/glazer_json_schema_test.erl`
- Possibly `c_src/glazer_schema_validator.cpp` for UTF-8 edge cases

**Test vectors**:
```erlang
% minLength: 2
"a" => error
"ab" => ok
"😀😀" => ok (2 graphemes even though 8 bytes)

% pattern: ^[a-z]+$
"abc" => ok
"123" => error
"" => error
```

**Estimated effort**: 1.5 days

---

#### Task 6: Array Validation (`items`, `minItems`, `maxItems`, `uniqueItems`) — 1.5 days
**Status**: Partially implemented; `items` is deferred
**What's left**: Recursive array element validation
- [x] `minItems`, `maxItems`
- [x] `uniqueItems` (basic)
- [ ] `items` schema (recursively validate each element)
- [ ] Test nested arrays
- [ ] Test uniqueItems with different types
- [ ] Test empty arrays

**Files to modify**:
- `c_src/glazer_schema_validator.cpp` — implement `check_items()`
- `c_src/glazer_schema_compiler.cpp` — recursively compile items schema
- `test/glazer_json_schema_test.erl`

**Implementation notes**:
```cpp
// In validate_value(), when encountering CHECK_ITEMS opcode:
// 1. Read items_schema_offset from bytecode
// 2. Get array length
// 3. For each element at index i:
//    - Push [i] to error path
//    - Call validate_value(element, items_schema_offset, ctx)
//    - Pop from error path
//    - If fail_fast and error, return false
```

**Test vectors**:
```erlang
% items: {type: integer}, minItems: 1, maxItems: 10
[] => error (too few)
[1, 2, 3] => ok
[1, "two", 3] => error (type mismatch at index 1)
[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11] => error (too many)
```

**Estimated effort**: 1.5 days

---

#### Task 7: Object Validation (`properties`, `required`, `additionalProperties`, `minProperties`, `maxProperties`) — 2 days
**Status**: Partially implemented; `properties` and `additionalProperties` deferred
**What's left**: Recursive object property validation
- [x] `minProperties`, `maxProperties`
- [ ] `required` (check all required keys present) — basic structure done, needs testing
- [ ] `properties` (validate each property against its schema)
- [ ] `additionalProperties` (reject or validate extra keys)
- [ ] Test nested objects
- [ ] Test missing required fields
- [ ] Test extra properties

**Files to modify**:
- `c_src/glazer_schema_validator.cpp` — implement `check_properties()` and `check_additional_properties()`
- `c_src/glazer_schema_compiler.cpp` — recursively compile properties schemas
- `test/glazer_json_schema_test.erl`

**Implementation notes**:
```cpp
// For properties:
// 1. Read properties_schema_offset from bytecode
// 2. For each property in data object:
//    - Get corresponding property schema from bytecode
//    - Push [property_key] to error path
//    - Call validate_value(property_value, property_schema_offset, ctx)
//    - Pop from error path

// For additionalProperties:
// 1. If additionalProperties = false:
//    - Check if any data keys NOT in properties list
//    - If found, emit error
// 2. If additionalProperties = schema:
//    - Validate extra keys against this schema
```

**Test vectors**:
```erlang
% properties: {name: string, age: integer}, required: [name]
{} => error (missing name)
{name => "Alice"} => ok
{name => "Alice", age => 30} => ok
{name => "Alice", age => "thirty"} => error (age type)
{name => "Alice", age => 30, extra => "field"} => ok (extra allowed by default)
```

**Estimated effort**: 2 days

---

#### Task 8: Simple Composition (`not`) — 1 day
**Status**: Not yet implemented
**What's left**: Invert validation logic
- [ ] Validate that term does NOT match schema
- [ ] Return error if schema matches
- [ ] Test with all constraint types

**Files to modify**:
- `c_src/glazer_schema_validator.cpp` — implement `check_not()`
- `c_src/glazer_schema_compiler.cpp` — compile not schema
- `test/glazer_json_schema_test.erl`

**Implementation notes**:
```cpp
// For NOT:
// 1. Read not_schema_offset from bytecode
// 2. Create temporary ValidationContext (don't affect parent)
// 3. Call validate_value(term, not_schema_offset, temp_ctx)
// 4. If temp_ctx is empty (no errors), then NOT validation FAILED
//    - Emit error in parent ctx
// 5. If temp_ctx has errors, then NOT validation PASSED
//    - Return true
```

**Test vectors**:
```erlang
% not: {type: string}
42 => ok (not a string)
"Alice" => error (is a string)

% not: {minimum: 100}
50 => ok (< 100)
150 => error (>= 100)
```

**Estimated effort**: 1 day

---

#### Task 9: Format Validation — 2 days
**Status**: Not yet implemented
**What's left**: Format validators for standard formats
- [ ] Built-in formats: `date`, `time`, `date-time`, `email`, `uri`, `uuid`, `ipv4`, `ipv6`, `hostname`
- [ ] Regex-based validation for each
- [ ] Custom format support via options
- [ ] Test all formats

**Files to modify**:
- `c_src/glazer_schema_validator.cpp` — implement format checking
- `c_src/glazer_schema_compiler.cpp` — compile format keyword
- `test/glazer_json_schema_test.erl`

**Implementation notes**:
```cpp
// Format validators (regex patterns):
static const std::unordered_map<std::string, std::string> FORMAT_PATTERNS = {
  {"email",     R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)"},
  {"ipv4",      R"(^(\d{1,3}\.){3}\d{1,3}$)"},
  {"ipv6",      R"(^([0-9a-fA-F]{0,4}:){2,7}[0-9a-fA-F]{0,4}$)"},
  {"uri",       R"(^[a-zA-Z][a-zA-Z0-9+\-.]*:)"},
  {"uuid",      R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)"},
  {"hostname",  R"(^([a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?\.)*[a-zA-Z]{2,}$)"},
};
```

**Test vectors**:
```erlang
% format: email
"alice@example.com" => ok
"invalid-email" => error
"test@domain" => ok (simplified)

% format: ipv4
"192.168.1.1" => ok
"256.1.1.1" => error (256 > 255)
"1.1.1" => error (only 3 octets)

% format: uuid
"550e8400-e29b-41d4-a716-446655440000" => ok
"invalid-uuid" => error
```

**Estimated effort**: 2 days

---

### Summary: Phase 2 Parallelization

**Developer allocation** (for 2-3 developers):
- Developer 1: Tasks 3-4 (2 days) → Task 8 (1 day) → Task 9 (2 days)
- Developer 2: Tasks 5-7 (5 days in parallel with Dev 1)
- Combined: ~5 days calendar time (overlapping work)

**Result**: All type and constraint keywords complete.

---

### Phase 3: Advanced Composition (Sequential) — 1.5-2 weeks

These require Task 2 to be solid, and may depend on prior advanced tasks.

#### Task 10: Composition Operators (`anyOf`, `oneOf`, `allOf`) — 3-4 days

#### Task 11: Schema References (`$ref`, `$defs`) — 3-4 days

#### Task 12: Conditional Validation (`if`/`then`/`else`) — 2-3 days

#### Task 13: Advanced Keywords (`prefixItems`, `dependentRequired`, `dependentSchemas`) — 1.5 days

*(Detailed implementation guides for Phase 3 available upon request)*

---

### Phase 4: Integration & Polish (1 week)

#### Task 14: JSON Schema Test Suite Integration — 2 days
#### Task 15: Performance Benchmarks — 2-3 days
#### Task 16: Documentation — 1 day

---

## How to Continue

### For Task 3-5 (Ready to implement):

1. **Start with tests**: Add test cases to `test/glazer_json_schema_test.erl`
2. **Verify existing code**: Run tests to confirm foundation works
3. **Add test vectors**: Use official JSON Schema test suite

### For Task 6-7 (Need recursion):

1. **Study the bytecode offset model**: See how `validate_value()` uses offsets
2. **Implement recursive calls**: Push/pop error paths
3. **Test deeply nested structures**

### For Task 8-9:

1. **Implement negation logic**: Invert validation result
2. **Add format validators**: Pre-compile regex patterns
3. **Test with real data**

### For Phase 3:

1. **Requires Tasks 0-7 stable**: Wait until full constraint set working
2. **Consider backtracking model**: For anyOf/oneOf composition
3. **Plan cycle detection**: For $ref references

---

## Key Implementation Patterns

### Error Path Tracking

```cpp
// Push when entering nested value
ctx.m_errors.back().path.push_back(enif_make_atom(env, property_key));

// Recursively validate
validate_value(nested_term, schema_offset, ctx);

// Pop when exiting (if not fail-fast)
ctx.m_errors.back().path.pop_back();
```

### Fail-Fast vs. Collect-All

```cpp
bool check_constraint(..., ValidationContext& ctx) {
  if (constraint_fails) {
    ctx.add_error(err);

    // Fail-fast: return immediately
    if (ctx.m_mode == ValidationMode::FAIL_FAST) return false;

    // Collect-all: continue to next constraint
    if (ctx.should_stop()) return false;  // Respect max_errors
    return true;  // Keep going
  }
  return true;
}
```

### Recursive Schema Compilation

```cpp
// Compile nested schema and store offset
size_t nested_schema_offset = m_bytecode.size();
bool ok = compile_schema_term(nested_schema_term, error_msg);

// Emit reference to nested schema bytecode
emit_opcode(SchemaOpcode::CHECK_ITEMS);
emit_varint(nested_schema_offset);
```

---

## Debugging Tips

### Bytecode Inspection

```erlang
% After compiling, inspect bytecode
{ok, {schema, CompiledSchema}} = glazer_json:compile_schema(Schema),
% CompiledSchema is a resource — can print via:
% io:format("~p~n", [CompiledSchema])
```

### Validation Tracing

```erlang
% Enable tracing in C++:
% Add to validate_value():
// std::cerr << "Validating at offset " << bytecode_offset << std::endl;

% Then run tests and observe output
```

### Error Path Debugging

```erlang
% Test with collect_all_errors to see all errors
{error, Errors} = glazer_json:validate(Schema, BadData, [collect_all_errors]),
lists:foreach(fun(Err) -> io:format("~p~n", [Err]) end, Errors).
```

---

## Success Criteria

### For each task:
- ✅ All constraint checks pass
- ✅ Edge cases covered (empty, null, max values)
- ✅ Fail-fast and collect-all modes both work
- ✅ Error paths are correct
- ✅ Passes official JSON Schema test vectors

### Overall:
- ✅ All 20 tasks implemented
- ✅ 1000+ official tests passing
- ✅ Performance target: 100k ops/sec (fail-fast), 50k ops/sec (collect-all)
- ✅ Documentation complete

---

## References

- Official JSON Schema: https://json-schema.org/draft/2020-12/
- Test Suite: https://github.com/json-schema-org/JSON-Schema-Test-Suite
- Glazer NIF Architecture: `c_src/glazer_nif.cpp`
- Validation Spec: `SCHEMA_VALIDATOR_SPEC.md`
- Implementation Progress: `SCHEMA_IMPLEMENTATION.md`
