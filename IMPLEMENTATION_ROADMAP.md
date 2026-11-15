# JSON Schema Validator — Implementation Roadmap

## Quick Start

### Current State
Phases 1-2 (Tasks 0-9) are **implemented and tested**:
- ✅ NIFs registered in module `glazer` and wired through `glazer_schema:compile_schema/1`, `validate/2,3`, `validate_term/2,3`
- ✅ Bytecode compiler: one program per (sub)schema, offset table, exact keyword dispatch
- ✅ Validation engine: type, numeric (int and float bounds), string, array (`items`, `prefixItems`), object (`properties`, `required`, `additionalProperties`, `dependentRequired`, `dependentSchemas`, min/max properties), `not`, `format`
- ✅ Phase 3: `anyOf`, `oneOf`, `allOf`, `if`/`then`/`else`
- ✅ References: `$ref` to any JSON Pointer (nested `$defs`, `~0`/`~1` escapes), `$anchor`, `$id` embedded resources, external documents via `compile_schema(Schema, [{refs, #{Uri => Schema}}])`, recursive schemas (depth-bounded)
- ✅ `$dynamicRef` / `$dynamicAnchor` with dynamic-scope resolution (outermost resource wins)
- ✅ `unevaluatedProperties` / `unevaluatedItems` (annotations flow through `$ref`, `allOf`, passing `anyOf`/`oneOf` branches, `if`/`then`/`else`, `dependentSchemas`)
- ✅ Also: `const`, `enum`, `contains`/`minContains`/`maxContains`, `patternProperties`, `propertyNames`, boolean schemas, `type` arrays, deep-equality `uniqueItems`
- ✅ Formats (asserted by default; `{format_assertion, false}` opts out): date-time, date, time, duration, email, idn-email, hostname, idn-hostname (IDNA2008, UTS 46 mapping, NFC, RFC 5893 bidi across the domain), ipv4, ipv6, uri, uri-reference, iri, iri-reference, uuid, json-pointer, relative-json-pointer, uri-template, regex
- ✅ Schema registry (`glazer_schema`) and `$id` naming: `compile_schema` registers by `$id` (or by the `name` option, or by content), `$ref` resolves from the registry, `{registry, false}` opts out
- ✅ `timeout_ms` returns `{error, {path, _, {timeout, Ms}}}` and never `ok`
- ✅ `compile_schema_file/1,2`
- ✅ Integers of any size: bounds, `multipleOf`, `const`/`enum` and `uniqueItems` compare exactly (`BigNum` in `glazer_bigint.hpp`)
- ✅ Mutually referring schemas: `glazer_schema:register_group/1,2` compiles a group together and stores it all or nothing
- ⏳ Not yet: remote fetching of documents (documents must be passed in), `$recursiveRef`, array-form `items` (draft 2019 and earlier), `dependencies`/`disallow`/`divisibleBy` (draft 3/4/7 legacy keywords).
- ✅ Performance measured (single process, 8-core WSL): about 320k ops/sec for valid documents and 500k for invalid ones with fail-fast, 270–330k with `collect_all_errors`. The targets (100k fail-fast, 50k collect-all) are met on this machine. Other hardware will differ.
- Scope and compliance: see the README's "Standards compliance" section and `guides/json-validation.md`

### Building & Testing

```bash
cd glazer
make optimize                      # rebuild the NIF (add REBUILD=1 to force)
rebar3 as test eunit --module=glazer_json_schema_test

# Expected: all EUnit tests pass (`make test`: 857 EUnit, 709 in-scope JSON Schema suite tests)
```

Note: `rebar3 eunit` does not always relink the NIF after a `c_src` change; use
`REBUILD=1 make -C c_src optimize` when the C++ sources changed.

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
{ok, {schema, CompiledSchema}} = glazer_schema:compile_schema(Schema),

% Validate in fail-fast mode (default)
ok = glazer_schema:validate(CompiledSchema, #{
  name => <<"Alice">>,
  age => 30
}),

% Validate with error collection
{error, Errors} = glazer_schema:validate(
  CompiledSchema,
  #{name => 42, age => 200},
  [collect_all_errors]
),

% Inline validation (compile + validate)
ok = glazer_schema:validate_term(Schema, #{name => <<"Bob">>, age => 25}).
```

---

## Task Breakdown & Implementation Order

### Phase 1: Foundation (COMPLETE) ✅

- [x] Task 0: NIF Framework
- [x] Task 1: Bytecode Compiler
- [x] Task 2: Validation Engine

**Status**: Complete. Basic constraint checks, error paths and both validation modes work.

---

### Phase 2: Type & Constraint Tasks (Implemented — covered by `test/glazer_json_schema_test.erl`)

**Status**: Tasks 3-9 are implemented and their tests pass. The checklists below are the original plan.

#### Task 3: Type Checking (`type`) — 1 day
**Status**: Already implemented in Task 2
**What's left**: Edge cases and tests
- [x] Basic type checking (bitmask)
- [x] Test all type combinations
- [x] Test invalid types in schema
- [x] Test "number" type (float OR integer)

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
- [x] Test with floats vs. integers
- [ ] Test bigints
- [x] Test edge cases (0, negative, max int64)

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
- [x] `minLength`, `maxLength` count code points (RFC 8259 characters), as JSON Schema 2020-12 requires. Grapheme counting is not used: it would change the result for decomposed characters.
- [x] `pattern` (regex matching via pre-compiled patterns)
- [x] Test UTF-8 multi-byte chars: "😀" = 1 code point, 4 bytes (`minLength` counts code points, not graphemes, as the spec requires)
- [x] Test regex edge cases (empty, special chars, escapes)
- [x] Test pattern errors in schema

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
- [x] `items` schema (recursively validate each element)
- [x] Test nested arrays
- [x] Test uniqueItems with different types
- [x] Test empty arrays

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
- [x] `required` (check all required keys present) — basic structure done, needs testing
- [x] `properties` (validate each property against its schema)
- [x] `additionalProperties` (reject or validate extra keys)
- [x] Test nested objects
- [x] Test missing required fields
- [x] Test extra properties

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
**Status**: Implemented (`not` runs in its own context that inherits the deadline and reports timeouts)
- [x] Validate that term does NOT match schema
- [x] Return error if schema matches
- [ ] Test with all constraint types (partly covered by `test/glazer_json_schema_test.erl`)

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
**Status**: Implemented, beyond the original list (see "Formats" under Current State and the README's Standards compliance section)
- [x] Built-in formats: `date`, `time`, `date-time`, `email`, `uri`, `uuid`, `ipv4`, `ipv6`, `hostname`, and the others listed under Current State
- [x] Hand-written validators for each (no regex dependency), with `regex` as its own format
- [x] Custom format support via options: `{formats, #{Name => Pattern}}`, checked as regular expressions (`CHECK_CUSTOM_FORMAT`)
- [x] Suite coverage: all in-scope format groups pass; `format-annotation` groups use `format_assertion: false`

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

**Status**: Runner `test/json_schema_suite.es`, run with `make test-jsts`. The Makefile fetches json-schema-org/JSON-Schema-Test-Suite at a pinned commit (`38dea93`, no upstream tag yet; requested in json-schema-org/JSON-Schema-Test-Suite#1278, to be switched to a `JSTS_TAG` once tagged) on first use, or uses a local checkout via `JSTS_DIR`. The in-scope results do not depend on the suite's `remotes/` folder, which upstream has removed; groups that need remote documents supply them through `externalSchemas`. Scope: JSON Schema 2020-12, format as annotation only. Excluded and reported separately: draft-specific tests (legacy keywords, draft 3/4/6/7/2019 forms, `$recursiveRef`), `optional/` features, format assertions, proposals/vocabularies.

In scope (2020-12: groups whose `compatibility` covers 2020, with no legacy keywords): **709 pass, 0 fail**. Excluded, counted separately in `test/json_schema_suite.es`:
- `draft_specific` (956), `legacy` (240), `upstream_issue` (33 tests in 15 groups, json-schema-org/JSON-Schema-Test-Suite#1277), ECMAScript-only regex (86), bignums (35), annotation-only content (10), optional (49), proposals (48).

Formats: asserted by default. `compile_schema(Schema, [{format_assertion, false}])` treats `format` as an annotation, as 2020-12 specifies for its default vocabulary. The suite's `format-annotation` groups use that option. Implemented in `c_src/glazer_formats.hpp`: date-time (with leap seconds and RFC 9557 suffixes), date, time, email, hostname (RFC 1123), ipv4 (RFC 2673, no leading zeros), ipv6, uri, uri-reference, iri, iri-reference, uuid, duration, json-pointer, relative-json-pointer, uri-template, regex (PCRE2 or SRELL, see `c_src/glazer_regex.hpp`), and idn-hostname (IDNA2008 with tables generated from ICU by `c_src/tools/gen_idna_tables.cpp` into `c_src/glazer_idna_tables.hpp`). Unknown formats pass.
Also implemented: `idn-email` (RFC 6531: UTF-8 local part, IDNA2008 domain), the UTS 46 mapping (generated from ICU) applied to internationalized domains before the IDNA2008 rules, and NFC normalization (UAX #15; the NFC step of UTS 46 and the RFC 5891 section 5.4 check on U-labels). Length limits: 63 octets per label (A-label form) and 253 for the whole name.
`format-idn-hostname.json` and `format-idn-email.json` pass completely. The RFC 5893 bidi rule is applied across the domain (a Bidi domain is one with at least one RTL label; every label must then satisfy rules 1-6). The idn-hostname and idn-email groups are compatibility 7, outside the 2020-12 scope.

Earlier totals (1185, 436 and 423 pass) are superseded.

Also done since Task 14 was first written:
- **`timeout_ms`**: a validation cut short by the deadline returns `{error, {path, Path, {timeout, Ms}}}`; it is never `ok`, including inside `not`. Errors recorded after the deadline are dropped.
- **Schema registry** (`glazer_schema`): named, versioned, persistent_term-backed; `register/1,2,3`, `fetch`, `validate`, `unregister`, `list`, `version`, `clear_interned`. Process-free.
- **Naming and `$ref`**: `compile_schema` registers a schema with a top-level `$id` under that `$id` by default, and `$ref`s to registered documents are resolved from the registry (transitively; explicit `refs` take precedence). A `{name, Name}` option names a schema without `$id`; a name that differs from the `$id` is an error, with or without `{registry, false}`.
- **Default registration**: schemas without `$id` are interned by content; `{registry, false}` opts out.
- Guide: `guides/json-validation.md` covers the API, formats, limits and the registry.

Schema regular expressions use PCRE2 when `libpcre2-8` is installed (detected by `c_src/Makefile`). Otherwise they use SRELL, a header-only engine fetched at tag `v2026.10` with SHA-256 checks into `c_src/vendors/srell` (see its README). Both are wrapped in `glazer_regex.hpp`. Known open issue: catastrophic backtracking on nested-quantifier patterns; PCRE2 has a match limit, SRELL has none.

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
{ok, {schema, CompiledSchema}} = glazer_schema:compile_schema(Schema),
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
{error, Errors} = glazer_schema:validate(Schema, BadData, [collect_all_errors]),
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
- ✅ Tasks 0–14 implemented (Task 14: the 2020-12 in-scope suite passes, 709 of 709)
- ✅ Unit tests: 857 EUnit tests passing (`make test`)
- ✅ Performance target (100k ops/sec fail-fast, 50k ops/sec collect-all): met on the measurement machine
- ✅ Documentation: `guides/json-validation.md`, README JSON Schema section, module edoc
- ⏳ Open items: decimal JSON numbers with more precision than a double; floating-point `multipleOf` uses a 1e-9 relative tolerance; regex engine errors carry the engine's code (SRELL: `-1`) and a JIT stack-limit error is not retried without JIT; validation costs about 50 ns per array element; property names are cached per validation, so only the first object in an array builds each key. All are listed in `guides/json-validation.md` under Known gaps.

---

## References

- Official JSON Schema: https://json-schema.org/draft/2020-12/
- Test Suite: https://github.com/json-schema-org/JSON-Schema-Test-Suite
- Glazer NIF Architecture: `c_src/glazer_nif.cpp`
- Validation Spec: `SCHEMA_VALIDATOR_SPEC.md`
- Implementation Progress: `SCHEMA_IMPLEMENTATION.md`
