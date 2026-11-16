// vim:ts=2:sw=2:et
//-----------------------------------------------------------------------------
// JSON Schema validation engine - implementation
//
// This file contains method implementations for SchemaValidator class.
// It is included at the end of glazer_schema_validator.hpp within namespace glz.
// Do not include this directly.
//-----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <limits>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "glazer_atoms.hpp"
#include "glazer_formats.hpp"
#include "glazer_json_schema_validator.hpp"

namespace glz {

std::pair<bool, ERL_NIF_TERM> SchemaValidator::validate(
  ERL_NIF_TERM   term,
  ValidationMode mode,
  size_t         max_depth,
  size_t         max_errors,
  int            timeout_ms
) {
  ValidationContext  ctx{
    .m_env        = m_env,
    .m_mode       = mode,
    .m_max_errors = max_errors,
    .m_max_depth  = max_depth,
    .m_start_time = std::chrono::steady_clock::now(),
    .m_timeout_ms = std::chrono::milliseconds(timeout_ms)
  };

  Evaluated root_eval;
  ctx.m_eval = m_schema.track_annotations ? &root_eval : nullptr;
  ctx.m_dyn_scope.push_back(0);   // the root resource

  validate_value(term, 0, ctx);

  // A walk cut short by timeout_ms has not checked everything, so it can never
  // be reported as valid. The timeout error is appended after any genuine errors.
  if (ctx.m_timed_out)
    ctx.m_errors.push_back(ValidationError{
      .reason        = ErrorReason::TIMEOUT,
      .reason_detail = enif_make_int(m_env, static_cast<int>(ctx.m_timeout_ms.count()))
    });

  if (ctx.m_errors.empty())
    return {true, AM_OK};

  auto ret = (mode == ValidationMode::FAIL_FAST)
           ? make_error_tuple(ctx.m_errors[0])
           : make_error_list(ctx.m_errors);
  return {true, ret};
}

bool SchemaValidator::validate_value(
  ERL_NIF_TERM term,
  size_t bytecode_offset,
  ValidationContext& ctx
) {
  if (ctx.is_timeout_exceeded())
    return true;

  if (ctx.m_current_depth >= ctx.m_max_depth) {
    ctx.add_error(ValidationError{
      .reason = ErrorReason::MAX_DEPTH_EXCEEDED
    });
    return false;
  }

  const uint8_t* bytecode = m_schema.bytecode.data();
  size_t bytecode_len = m_schema.bytecode.size();

  while (bytecode_offset < bytecode_len) {
    auto opcode = static_cast<SchemaOpcode>(bytecode[bytecode_offset++]);

    if (opcode == SchemaOpcode::SCHEMA_END)
      break;

    bool result = true;
    switch (opcode) {
      case SchemaOpcode::CHECK_TYPE: {
        uint64_t type_mask = 0;
        if (!read_varint(bytecode_offset, type_mask)) break;
        result = check_type(term, static_cast<uint8_t>(type_mask), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MINIMUM: {
        uint64_t idx = 0;
        if (!read_varint(bytecode_offset, idx) || idx >= m_schema.numbers.size()) break;
        result = check_minimum(term, m_schema.numbers[idx], ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAXIMUM: {
        uint64_t idx = 0;
        if (!read_varint(bytecode_offset, idx) || idx >= m_schema.numbers.size()) break;
        result = check_maximum(term, m_schema.numbers[idx], ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_LENGTH: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_length(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_LENGTH: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_length(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_PATTERN: {
        uint64_t pattern_idx = 0;
        if (!read_varint(bytecode_offset, pattern_idx)) break;
        result = check_pattern(term, static_cast<size_t>(pattern_idx), ctx);
        break;
      }
      case SchemaOpcode::CHECK_CUSTOM_FORMAT: {
        uint64_t pattern_idx = 0;
        if (!read_varint(bytecode_offset, pattern_idx)) break;
        result = check_custom_format(term, static_cast<size_t>(pattern_idx), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_ITEMS: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_items(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_ITEMS: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_items(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MIN_PROPERTIES: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_min_properties(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_MAX_PROPERTIES: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len)) break;
        result = check_max_properties(term, static_cast<size_t>(len), ctx);
        break;
      }
      case SchemaOpcode::CHECK_EXCLUSIVE_MINIMUM: {
        uint64_t idx = 0;
        if (!read_varint(bytecode_offset, idx) || idx >= m_schema.numbers.size()) break;
        result = check_exclusive_minimum(term, m_schema.numbers[idx], ctx);
        break;
      }
      case SchemaOpcode::CHECK_EXCLUSIVE_MAXIMUM: {
        uint64_t idx = 0;
        if (!read_varint(bytecode_offset, idx) || idx >= m_schema.numbers.size()) break;
        result = check_exclusive_maximum(term, m_schema.numbers[idx], ctx);
        break;
      }
      case SchemaOpcode::CHECK_MULTIPLE_OF: {
        uint64_t idx = 0;
        if (!read_varint(bytecode_offset, idx) || idx >= m_schema.numbers.size()) break;
        result = check_multiple_of(term, m_schema.numbers[idx], ctx);
        break;
      }
      case SchemaOpcode::CHECK_UNIQUE_ITEMS: {
        result = check_unique_items(term, ctx);
        break;
      }
      case SchemaOpcode::CHECK_ITEMS: {
        size_t items_off = 0;
        uint64_t start = 0;
        if (!read_program(bytecode_offset, items_off)) break;
        if (!read_varint(bytecode_offset, start)) break;
        result = check_items(term, items_off, static_cast<size_t>(start), ctx);
        break;
      }
      case SchemaOpcode::CHECK_PREFIX_ITEMS: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<size_t> offsets;
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          size_t off = 0;
          parsed = read_program(bytecode_offset, off);
          if (parsed) offsets.push_back(off);
        }
        if (!parsed) break;
        result = check_prefix_items(term, offsets, ctx);
        break;
      }
      case SchemaOpcode::CHECK_ANY_OF:
      case SchemaOpcode::CHECK_ONE_OF:
      case SchemaOpcode::CHECK_ALL_OF: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<size_t> offsets;
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          size_t off = 0;
          parsed = read_program(bytecode_offset, off);
          if (parsed) offsets.push_back(off);
        }
        if (!parsed) break;
        if (opcode == SchemaOpcode::CHECK_ANY_OF)      result = check_any_of(term, offsets, ctx);
        else if (opcode == SchemaOpcode::CHECK_ONE_OF) result = check_one_of(term, offsets, ctx);
        else                                           result = check_all_of(term, offsets, ctx);
        break;
      }
      case SchemaOpcode::CHECK_REF: {
        size_t target = 0;
        if (!read_program(bytecode_offset, target)) break;
        // Depth grows with each reference, so recursive schemas terminate
        ++ctx.m_current_depth;
        validate_applicator(term, target, ctx);
        --ctx.m_current_depth;
        result = ctx.m_errors.empty();
        break;
      }
      case SchemaOpcode::CHECK_IF_THEN_ELSE: {
        size_t if_off = 0, then_off = NO_BRANCH, else_off = NO_BRANCH;
        uint64_t then_code = 0, else_code = 0;
        if (!read_program(bytecode_offset, if_off)) break;
        if (!read_varint(bytecode_offset, then_code)) break;
        if (!read_varint(bytecode_offset, else_code)) break;
        if (then_code > 0) {
          if (then_code > m_schema.program_offsets.size()) break;
          then_off = m_schema.program_offsets[then_code - 1];
        }
        if (else_code > 0) {
          if (else_code > m_schema.program_offsets.size()) break;
          else_off = m_schema.program_offsets[else_code - 1];
        }
        result = check_if_then_else(term, if_off, then_off, else_off, ctx);
        break;
      }
      case SchemaOpcode::CHECK_DEPENDENT_REQUIRED: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::pair<std::string, std::vector<std::string>>> deps;
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          std::string name;
          uint64_t k = 0;
          parsed = read_string(bytecode_offset, name) && read_varint(bytecode_offset, k);
          std::vector<std::string> required;
          for (uint64_t j = 0; j < k && parsed; ++j) {
            std::string field;
            parsed = read_string(bytecode_offset, field);
            if (parsed) required.push_back(std::move(field));
          }
          if (parsed) deps.emplace_back(std::move(name), std::move(required));
        }
        if (!parsed) break;
        result = check_dependent_required(term, deps, ctx);
        break;
      }
      case SchemaOpcode::CHECK_DEPENDENT_SCHEMAS: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::pair<std::string, size_t>> deps;
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          std::string name;
          size_t off = 0;
          parsed = read_string(bytecode_offset, name) && read_program(bytecode_offset, off);
          if (parsed) deps.emplace_back(std::move(name), off);
        }
        if (!parsed) break;
        result = check_dependent_schemas(term, deps, ctx);
        break;
      }
      case SchemaOpcode::CHECK_REQUIRED: {
        uint64_t num_fields = 0;
        if (!read_varint(bytecode_offset, num_fields)) break;
        std::vector<std::string_view> required_fields;
        required_fields.reserve(num_fields);
        bool parsed = true;
        for (uint64_t i = 0; i < num_fields && parsed; ++i) {
          std::string_view field;
          parsed = read_string_view(bytecode_offset, field);
          if (parsed) required_fields.push_back(field);
        }
        if (!parsed) break;
        result = check_required(term, required_fields, ctx);
        break;
      }
      case SchemaOpcode::CHECK_PROPERTIES: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::pair<std::string_view, size_t>> props;
        props.reserve(count);
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          std::string_view name;
          uint64_t prog = 0;
          parsed = read_string_view(bytecode_offset, name)
                && read_varint(bytecode_offset, prog)
                && prog < m_schema.program_offsets.size();
          if (parsed)
            props.emplace_back(name, m_schema.program_offsets[prog]);
        }
        if (!parsed) break;
        result = check_properties(term, props, ctx);
        break;
      }
      case SchemaOpcode::CHECK_ADDITIONAL_PROPERTIES: {
        uint64_t kind = 0, count = 0;
        if (!read_varint(bytecode_offset, kind)) break;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::string_view> declared;
        declared.reserve(count);
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          std::string_view name;
          parsed = read_string_view(bytecode_offset, name);
          if (parsed) declared.push_back(name);
        }
        uint64_t pattern_count = 0;
        parsed = parsed && read_varint(bytecode_offset, pattern_count);
        std::vector<size_t> patterns;
        for (uint64_t i = 0; i < pattern_count && parsed; ++i) {
          uint64_t pidx = 0;
          parsed = read_varint(bytecode_offset, pidx);
          if (parsed) patterns.push_back(static_cast<size_t>(pidx));
        }
        size_t schema_offset = 0;
        if (parsed && kind == 1) {
          uint64_t prog = 0;
          parsed = read_varint(bytecode_offset, prog) && prog < m_schema.program_offsets.size();
          if (parsed) schema_offset = m_schema.program_offsets[prog];
        }
        if (!parsed) break;
        result = check_additional_properties(term, static_cast<int>(kind), declared, patterns, schema_offset, ctx);
        break;
      }
      case SchemaOpcode::CHECK_NOT: {
        uint64_t prog = 0;
        if (!read_varint(bytecode_offset, prog) || prog >= m_schema.program_offsets.size()) break;
        result = check_not(term, m_schema.program_offsets[prog], ctx);
        break;
      }
      case SchemaOpcode::CHECK_DYNAMIC_REF: {
        size_t fallback = 0;
        std::string anchor;
        if (!read_program(bytecode_offset, fallback)) break;
        if (!read_string(bytecode_offset, anchor)) break;

        // The outermost resource in the dynamic scope that defines the anchor wins
        size_t target = fallback;
        for (size_t scope_off : ctx.m_dyn_scope) {
          auto doc = m_schema.resource_of.find(scope_off);
          if (doc == m_schema.resource_of.end())
            continue;
          auto anchors = m_schema.dynamic_anchors.find(doc->second);
          if (anchors == m_schema.dynamic_anchors.end())
            continue;
          auto found = anchors->second.find(anchor);
          if (found != anchors->second.end()) {
            target = found->second;
            break;
          }
        }

        ++ctx.m_current_depth;
        validate_applicator(term, target, ctx);
        --ctx.m_current_depth;
        result = ctx.m_errors.empty();
        break;
      }
      case SchemaOpcode::CHECK_UNEVALUATED_PROPERTIES:
      case SchemaOpcode::CHECK_UNEVALUATED_ITEMS: {
        uint64_t kind = 0;   // 0 = forbidden, 1 = schema, 2 = true (everything evaluated)
        size_t schema_off = 0;
        if (!read_varint(bytecode_offset, kind)) break;
        if (kind == 1 && !read_program(bytecode_offset, schema_off)) break;
        if (kind == 2) {
          if (ctx.m_eval) {
            if (opcode == SchemaOpcode::CHECK_UNEVALUATED_PROPERTIES) ctx.m_eval->all_props = true;
            else ctx.m_eval->items = std::numeric_limits<size_t>::max();
          }
          break;
        }
        if (opcode == SchemaOpcode::CHECK_UNEVALUATED_PROPERTIES)
          result = check_unevaluated_properties(term, kind == 0, schema_off, ctx);
        else
          result = check_unevaluated_items(term, kind == 0, schema_off, ctx);
        break;
      }
      case SchemaOpcode::CHECK_CONST: {
        uint64_t len = 0;
        if (!read_varint(bytecode_offset, len) || len > m_schema.bytecode.size() - bytecode_offset) break;
        result = check_const(term, bytecode_offset, static_cast<size_t>(len), ctx);
        bytecode_offset += static_cast<size_t>(len);
        break;
      }
      case SchemaOpcode::CHECK_ENUM: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::pair<size_t, size_t>> values;   // (offset, length)
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          uint64_t len = 0;
          parsed = read_varint(bytecode_offset, len) && len <= m_schema.bytecode.size() - bytecode_offset;
          if (parsed) {
            values.emplace_back(bytecode_offset, static_cast<size_t>(len));
            bytecode_offset += static_cast<size_t>(len);
          }
        }
        if (!parsed) break;
        result = check_enum(term, values, ctx);
        break;
      }
      case SchemaOpcode::CHECK_CONTAINS: {
        size_t sub = 0;
        uint64_t min_count = 0, max_code = 0;
        if (!read_program(bytecode_offset, sub)) break;
        if (!read_varint(bytecode_offset, min_count)) break;
        if (!read_varint(bytecode_offset, max_code)) break;
        result = check_contains(term, sub, static_cast<size_t>(min_count),
                                max_code > 0 ? static_cast<size_t>(max_code - 1) : 0,
                                max_code > 0, ctx);
        break;
      }
      case SchemaOpcode::CHECK_PATTERN_PROPERTIES: {
        uint64_t count = 0;
        if (!read_varint(bytecode_offset, count)) break;
        std::vector<std::pair<size_t, size_t>> entries;
        bool parsed = true;
        for (uint64_t i = 0; i < count && parsed; ++i) {
          uint64_t pidx = 0;
          size_t off = 0;
          parsed = read_varint(bytecode_offset, pidx) && read_program(bytecode_offset, off);
          if (parsed) entries.emplace_back(static_cast<size_t>(pidx), off);
        }
        if (!parsed) break;
        result = check_pattern_properties(term, entries, ctx);
        break;
      }
      case SchemaOpcode::CHECK_PROPERTY_NAMES: {
        size_t off = 0;
        if (!read_program(bytecode_offset, off)) break;
        result = check_property_names(term, off, ctx);
        break;
      }
      case SchemaOpcode::CHECK_FORMAT: {
        std::string_view format;
        if (!read_string_view(bytecode_offset, format)) break;
        // Format check passed?
        if (check_format_value(m_env, term, format))
          result = true;
        else {
          ctx.add_error(ValidationError{
            .reason       = ErrorReason::FORMAT_INVALID,
            .actual_value = term
          });
          result = false;
        }
        break;
      }
      default:
        break;
    }

    if (!result && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;

    if (ctx.should_stop())
      return false;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_type(ERL_NIF_TERM term, uint8_t type_mask, ValidationContext& ctx) {
  auto term_type = term_to_type(m_env, term);
  if (term_type == JSONType::INVALID) [[unlikely]] {
    ctx.add_error(ValidationError{
      .path         = ctx.get_last_path(),
      .reason       = ErrorReason::INVALID_TERM_TYPE,
      .actual_value = term
    });
    return false;
  }

  auto term_type_mask = static_cast<uint8_t>(term_type);

  // 1.0 is an integer in JSON Schema terms: accept integral floats for "integer"
  if (term_type == JSONType::FLOAT && (type_mask & static_cast<uint8_t>(JSONType::INTEGER))) {
    double d = 0;
    if (get_number_double(term, d) && std::isfinite(d) && d == std::trunc(d))
      return true;
  }

  if ((term_type_mask & type_mask) == 0) {
    ctx.add_error(ValidationError{
      .path         = ctx.get_last_path(),
      .reason       = ErrorReason::TYPE_MISMATCH,
      .actual_value = term
    });
    return false;
  }
  return true;
}

// Compares a numeric term with a bound: -1, 0 or 1. False when term is not a
// number. Integers compare exactly, against integer and float bounds alike.
bool SchemaValidator::compare_to_bound(ERL_NIF_TERM term, const Number& bound, int& cmp) {
  constexpr int64_t EXACT_DOUBLE = int64_t(1) << 53;   // every integer up to here is a double
  ErlNifSInt64 lv = 0;
  if (enif_get_int64(m_env, term, &lv)) {
    int64_t v = static_cast<int64_t>(lv);
    if (const auto* bi = std::get_if<int64_t>(&bound)) {
      cmp = v < *bi ? -1 : (v > *bi ? 1 : 0);
    } else if (const auto* bd = std::get_if<double>(&bound)) {
      if (v <= EXACT_DOUBLE && v >= -EXACT_DOUBLE) {
        double dv = static_cast<double>(v);
        cmp = dv < *bd ? -1 : (dv > *bd ? 1 : 0);
      } else {
        cmp = BigNum::compare_double(BigNum::from_int64(v), *bd);
      }
    } else {   // a BigNum bound: compared in place, without a copy
      cmp = BigNum::compare(BigNum::from_int64(v), std::get<BigNum>(bound));
    }
    return true;
  }

  BigNum big;
  if (BigNum::from_term(m_env, term, big)) {
    if (const auto* bd = std::get_if<double>(&bound))
      cmp = BigNum::compare_double(big, *bd);
    else if (const auto* bi = std::get_if<int64_t>(&bound))
      cmp = BigNum::compare(big, BigNum::from_int64(*bi));
    else
      cmp = BigNum::compare(big, std::get<BigNum>(bound));
    return true;
  }

  double v = 0;
  if (!enif_get_double(m_env, term, &v)) return false;
  if (const auto* bd = std::get_if<double>(&bound))
    cmp = v < *bd ? -1 : (v > *bd ? 1 : 0);
  else if (const auto* bi = std::get_if<int64_t>(&bound))
    cmp = -BigNum::compare_double(BigNum::from_int64(*bi), v);
  else
    cmp = -BigNum::compare_double(std::get<BigNum>(bound), v);
  return true;
}

bool SchemaValidator::check_minimum(ERL_NIF_TERM term, const Number& min_val, ValidationContext& ctx) {
  int cmp = 0;
  if (!compare_to_bound(term, min_val, cmp) || cmp >= 0) [[likely]]
    return true;

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::VALUE_TOO_SMALL,
    .reason_detail = number_to_term(m_env, min_val),
    .actual_value  = term
  });
  return false;
}

bool SchemaValidator::check_maximum(ERL_NIF_TERM term, const Number& max_val, ValidationContext& ctx) {
  int cmp = 0;
  if (!compare_to_bound(term, max_val, cmp) || cmp <= 0) [[likely]]
    return true;

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::VALUE_TOO_LARGE,
    .reason_detail = number_to_term(m_env, max_val),
    .actual_value  = term
  });
  return false;
}

bool SchemaValidator::check_exclusive_minimum(ERL_NIF_TERM term, const Number& min_val, ValidationContext& ctx) {
  int cmp = 0;
  if (!compare_to_bound(term, min_val, cmp) || cmp > 0) [[likely]]
    return true;

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::VALUE_TOO_SMALL,
    .reason_detail = number_to_term(m_env, min_val),
    .actual_value  = term
  });
  return false;
}

bool SchemaValidator::check_exclusive_maximum(ERL_NIF_TERM term, const Number& max_val, ValidationContext& ctx) {
  int cmp = 0;
  if (!compare_to_bound(term, max_val, cmp) || cmp < 0) [[likely]]
    return true;

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::VALUE_TOO_LARGE,
    .reason_detail = number_to_term(m_env, max_val),
    .actual_value  = term
  });
  return false;
}

// Whether the integer term is a multiple of an integer factor. False when term
// is not an integer. The factor is not zero.
bool SchemaValidator::integer_multiple(ERL_NIF_TERM term, const Number& factor, bool& multiple) {
  ErlNifSInt64 lv = 0;
  if (const auto* fi = std::get_if<int64_t>(&factor); fi && enif_get_int64(m_env, term, &lv)) {
    // int64 by int64: magnitudes in uint64 avoid overflow at INT64_MIN
    uint64_t mag = lv < 0 ? uint64_t(0) - static_cast<uint64_t>(lv) : static_cast<uint64_t>(lv);
    uint64_t d   = *fi < 0 ? uint64_t(0) - static_cast<uint64_t>(*fi) : static_cast<uint64_t>(*fi);
    multiple = mag % d == 0;
    return true;
  }

  BigNum value;
  if (!BigNum::from_term(m_env, term, value)) return false;
  if (const auto* fi = std::get_if<int64_t>(&factor)) {
    uint64_t d = *fi < 0 ? uint64_t(0) - static_cast<uint64_t>(*fi) : static_cast<uint64_t>(*fi);
    multiple = BigNum::mod_u64(value, d) == 0;
  } else {
    multiple = BigNum::mod(value, std::get<BigNum>(factor)).is_zero();
  }
  return true;
}

bool SchemaValidator::check_multiple_of(ERL_NIF_TERM term, const Number& factor, ValidationContext& ctx) {
  if (const auto* fi = std::get_if<int64_t>(&factor); fi && *fi == 0) [[unlikely]]
    return true;   // a zero factor does not constrain (the spec requires it to be positive)

  bool multiple = false;
  if (!std::holds_alternative<double>(factor) && integer_multiple(term, factor, multiple)) {
    if (multiple) return true;
    ctx.add_error(ValidationError{
      .reason        = ErrorReason::NOT_MULTIPLE_OF,
      .reason_detail = number_to_term(m_env, factor),
      .actual_value  = term
    });
    return false;
  }

  // floats: a relative tolerance on the quotient
  double v = 0;
  BigNum value;
  if (BigNum::from_term(m_env, term, value))
    v = value.to_double();
  else if (!get_number_double(term, v))
    return true;

  double f = number_approx(factor);
  if (f == 0.0) [[likely]]
    return true;

  // A finite quotient is tested with a relative tolerance. An overflowing one
  // cannot be rounded, so the test is exact instead: fmod is exact for doubles.
  double q = v / f;
  bool is_multiple = std::isfinite(q)
    ? std::fabs(q - std::nearbyint(q)) <= 1e-9 * std::max(1.0, std::fabs(q))
    : std::fmod(v, f) == 0.0;
  if (is_multiple)
    return true;

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::NOT_MULTIPLE_OF,
    .reason_detail = number_to_term(m_env, factor),
    .actual_value  = term
  });
  return false;
}

bool SchemaValidator::check_min_length(ERL_NIF_TERM term, size_t min_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;   // length applies to strings only

  size_t char_count = 0;
  auto data = bin.data;
  auto end  = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80)
      char_count++;
  }

  if (char_count >= min_len)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::STRING_TOO_SHORT,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_max_length(ERL_NIF_TERM term, size_t max_len, ValidationContext& ctx) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  size_t char_count = 0;
  auto data = bin.data;
  auto end  = data + bin.size;
  while (data < end) {
    uint8_t byte = *data++;
    if ((byte & 0xC0) != 0x80)
      char_count++;
  }

  if (char_count <= max_len)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::STRING_TOO_LONG,
    .actual_value = term
  });
  return false;
}

// The error for a regex engine failure (not a mismatch): the engine's code is the detail
inline ValidationError regex_failure(ErlNifEnv* env, ERL_NIF_TERM term, int code) {
  return ValidationError{
    .reason        = ErrorReason::REGEX_ERROR,
    .reason_detail = enif_make_int(env, code),
    .actual_value  = term
  };
}

bool SchemaValidator::check_pattern(
  ERL_NIF_TERM term,
  size_t pattern_idx,
  ValidationContext& ctx
) {
  if (pattern_idx >= m_schema.patterns.size())
    return true;

  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  const auto& pattern = m_schema.patterns[pattern_idx];
  std::string_view text(reinterpret_cast<const char*>(bin.data), bin.size);

  // Regex matching can be expensive — report work to scheduler
  update_reduction_count(m_env, bin.size);

  RegexMatch outcome = pattern->match(text);
  if (outcome.status == RegexMatch::Status::Error) {
    ctx.add_error(regex_failure(m_env, term, outcome.code));
    return false;
  }
  if (outcome.status == RegexMatch::Status::NoMatch) {
    ctx.add_error(ValidationError{
      .reason       = ErrorReason::PATTERN_MISMATCH,
      .actual_value = term
    });
    return false;
  }
  return true;
}

// A custom format: a string value must match the pattern. Non-strings pass.
bool SchemaValidator::check_custom_format(ERL_NIF_TERM term, size_t pattern_idx, ValidationContext& ctx) {
  if (pattern_idx >= m_schema.patterns.size())
    return true;

  ErlNifBinary bin;
  if (!enif_inspect_binary(m_env, term, &bin))
    return true;

  std::string_view text(reinterpret_cast<const char*>(bin.data), bin.size);
  update_reduction_count(m_env, bin.size);
  RegexMatch outcome = m_schema.patterns[pattern_idx]->match(text);
  if (outcome.status == RegexMatch::Status::Error) {
    ctx.add_error(regex_failure(m_env, term, outcome.code));
    return false;
  }
  if (outcome.status == RegexMatch::Status::NoMatch) {
    ctx.add_error(ValidationError{
      .reason       = ErrorReason::FORMAT_INVALID,
      .actual_value = term
    });
    return false;
  }
  return true;
}

bool SchemaValidator::check_min_items(ERL_NIF_TERM term, size_t min_items, ValidationContext& ctx) {
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) [[unlikely]]
    return true;

  if (list_len >= min_items)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::TOO_FEW_ITEMS,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_max_items(ERL_NIF_TERM term, size_t max_items, ValidationContext& ctx) {
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0) [[unlikely]]
    return true;

  if (list_len <= max_items)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::TOO_MANY_ITEMS,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_unique_items(ERL_NIF_TERM term, ValidationContext& ctx) {
  if (!enif_is_list(m_env, term))
    return true;

  std::vector<ERL_NIF_TERM> seen;
  ERL_NIF_TERM head, tail = term;
  size_t index = 0;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    for (ERL_NIF_TERM prior : seen) {
      if (terms_equal(prior, head)) {
        ctx.add_error(ValidationError{
          .reason        = ErrorReason::DUPLICATE_ITEM,
          .reason_detail = enif_make_ulong(m_env, index),
          .actual_value  = head
        });
        return false;
      }
    }
    seen.push_back(head);
    ++index;
  }
  return true;
}

bool SchemaValidator::check_min_properties(
  ERL_NIF_TERM       term,
  size_t             min_props,
  ValidationContext& ctx
) {
  size_t map_size = 0;
  if (!enif_get_map_size(m_env, term, &map_size))
    return true;

  if (map_size >= min_props)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::TOO_FEW_PROPERTIES,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_max_properties(
  ERL_NIF_TERM term,
  size_t max_props,
  ValidationContext& ctx
) {
  size_t map_size = 0;
  if (!enif_get_map_size(m_env, term, &map_size))
    return true;

  if (map_size <= max_props)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::TOO_MANY_PROPERTIES,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::get_number_double(ERL_NIF_TERM term, double& out) {
  if (enif_get_double(m_env, term, &out))
    return true;

  ErlNifSInt64 long_val;
  if (enif_get_int64(m_env, term, &long_val)) {
    out = static_cast<double>(long_val);
    return true;
  }

  return false;
}

bool SchemaValidator::get_string(ERL_NIF_TERM term, std::string& out) {
  ErlNifBinary bin;
  if (enif_inspect_binary(m_env, term, &bin)) {
    out = std::string(reinterpret_cast<const char*>(bin.data), bin.size);
    return true;
  }
  // Map keys may be atoms
  char buf[256];
  if (enif_get_atom(m_env, term, buf, sizeof(buf), ERL_NIF_LATIN1)) {
    out = buf;
    return true;
  }
  return false;
}

// Snake-case atom naming each ErrorReason (the Erlang-visible reason tag)
[[maybe_unused]] static ERL_NIF_TERM reason_atom(ErrorReason r) {
  switch (r) {
    case ErrorReason::TYPE_MISMATCH:            return AM_TYPE_MISMATCH;
    case ErrorReason::VALUE_TOO_SMALL:          return AM_VALUE_TOO_SMALL;
    case ErrorReason::VALUE_TOO_LARGE:          return AM_VALUE_TOO_LARGE;
    case ErrorReason::NOT_MULTIPLE_OF:          return AM_NOT_MULTIPLE_OF;
    case ErrorReason::STRING_TOO_SHORT:         return AM_STRING_TOO_SHORT;
    case ErrorReason::STRING_TOO_LONG:          return AM_STRING_TOO_LONG;
    case ErrorReason::PATTERN_MISMATCH:         return AM_PATTERN_MISMATCH;
    case ErrorReason::MISSING_REQUIRED:         return AM_MISSING_REQUIRED;
    case ErrorReason::EXTRA_PROPERTIES:         return AM_EXTRA_PROPERTIES;
    case ErrorReason::TOO_FEW_ITEMS:            return AM_TOO_FEW_ITEMS;
    case ErrorReason::TOO_MANY_ITEMS:           return AM_TOO_MANY_ITEMS;
    case ErrorReason::DUPLICATE_ITEM:           return AM_DUPLICATE_ITEM;
    case ErrorReason::TOO_FEW_PROPERTIES:       return AM_TOO_FEW_PROPERTIES;
    case ErrorReason::TOO_MANY_PROPERTIES:      return AM_TOO_MANY_PROPERTIES;
    case ErrorReason::SCHEMA_MATCHED:           return AM_SCHEMA_MATCHED;
    case ErrorReason::NO_SCHEMA_MATCHED:        return AM_NO_SCHEMA_MATCHED;
    case ErrorReason::MULTIPLE_SCHEMAS_MATCHED: return AM_MULTIPLE_SCHEMAS_MATCHED;
    case ErrorReason::ALL_SCHEMAS_FAILED:       return AM_ALL_SCHEMAS_FAILED;
    case ErrorReason::ITEMS_VALIDATION_FAILED:  return AM_ITEMS_VALIDATION_FAILED;
    case ErrorReason::UNRESOLVED_REF:           return AM_UNRESOLVED_REF;
    case ErrorReason::CIRCULAR_REFERENCE:       return AM_CIRCULAR_REFERENCE;
    case ErrorReason::THEN_VALIDATION_FAILED:   return AM_THEN_VALIDATION_FAILED;
    case ErrorReason::ELSE_VALIDATION_FAILED:   return AM_ELSE_VALIDATION_FAILED;
    case ErrorReason::FORMAT_INVALID:           return AM_FORMAT_INVALID;
    case ErrorReason::DEPENDENT_KEYS_MISSING:   return AM_DEPENDENT_KEYS_MISSING;
    case ErrorReason::DEPENDENT_SCHEMA_FAILED:  return AM_DEPENDENT_SCHEMA_FAILED;
    case ErrorReason::EXPECTED_STRING:          return AM_EXPECTED_STRING;
    case ErrorReason::INVALID_TERM_TYPE:        return AM_INVALID_TERM_TYPE;
    case ErrorReason::MAX_DEPTH_EXCEEDED:       return AM_MAX_DEPTH_EXCEEDED;
    case ErrorReason::NOT_CONST:                return AM_NOT_CONST;
    case ErrorReason::NOT_IN_ENUM:              return AM_NOT_IN_ENUM;
    case ErrorReason::CONTAINS_FAILED:          return AM_CONTAINS_FAILED;
    case ErrorReason::TIMEOUT:                  return AM_TIMEOUT;
    case ErrorReason::REGEX_ERROR:              return AM_REGEX_ERROR;
    default:                                    return AM_UNKNOWN;
  }
}

// Erlang-visible error body: {path, Path, {Reason, Detail}}
// Path is root-first; the error's path vector is stored leaf-last.
ERL_NIF_TERM SchemaValidator::make_error_body(const ValidationError& err) {
  auto path_list = enif_make_list(m_env, 0);
  for (auto it = err.path.rbegin(); it != err.path.rend(); ++it)
    path_list = enif_make_list_cell(m_env, *it, path_list);

  // Unset term slots are 0 (invalid); fall back to the actual value, then nil.
  ERL_NIF_TERM detail = err.reason_detail;
  if (detail == 0) detail = err.actual_value;
  if (detail == 0) detail = AM_NIL;

  return enif_make_tuple3(
    m_env,
    AM_PATH,
    path_list,
    enif_make_tuple2(
      m_env,
      reason_atom(err.reason),
      detail
    )
  );
}

ERL_NIF_TERM SchemaValidator::make_error_tuple(const ValidationError& err) {
  return enif_make_tuple2(m_env, AM_ERROR, make_error_body(err));
}

template<typename ErrorVec>
ERL_NIF_TERM SchemaValidator::make_error_list(const ErrorVec& errors) {
  auto error_list = enif_make_list(m_env, 0);
  for (const auto& err : errors)
    error_list = enif_make_list_cell(m_env, make_error_body(err), error_list);

  return enif_make_tuple2(m_env, AM_ERROR, error_list);
}

ERL_NIF_TERM SchemaValidator::binary_term(std::string_view str) {
  ERL_NIF_TERM term;
  unsigned char* p = enif_make_new_binary(m_env, str.size(), &term);
  if (!str.empty())
    std::memcpy(p, str.data(), str.size());
  return term;
}

bool SchemaValidator::read_double(size_t& offset, double& out) {
  if (m_schema.bytecode.size() - offset < sizeof(double) || offset > m_schema.bytecode.size())
    return false;
  std::memcpy(&out, m_schema.bytecode.data() + offset, sizeof(double));
  offset += sizeof(double);
  return true;
}

// A map key as a view: binary keys are viewed in place, atom keys are copied
// into scratch. The view is valid while the key term and scratch are.
bool SchemaValidator::key_view(ERL_NIF_TERM key, std::string& scratch, std::string_view& out) {
  ErlNifBinary bin;
  if (enif_inspect_binary(m_env, key, &bin)) {
    out = std::string_view(reinterpret_cast<const char*>(bin.data), bin.size);
    return true;
  }
  char buf[256];
  if (enif_get_atom(m_env, key, buf, sizeof(buf), ERL_NIF_LATIN1)) {
    scratch = buf;
    out = scratch;
    return true;
  }
  return false;
}

bool SchemaValidator::read_string_view(size_t& offset, std::string_view& out) {
  uint64_t len = 0;
  if (!read_varint(offset, len))
    return false;
  if (len > m_schema.bytecode.size() - offset)
    return false;
  out = std::string_view(reinterpret_cast<const char*>(m_schema.bytecode.data() + offset), len);
  offset += len;
  return true;
}

bool SchemaValidator::read_string(size_t& offset, std::string& out) {
  uint64_t len = 0;
  if (!read_varint(offset, len))
    return false;
  if (len > m_schema.bytecode.size() - offset)
    return false;
  out.assign(reinterpret_cast<const char*>(m_schema.bytecode.data() + offset), len);
  offset += len;
  return true;
}

bool SchemaValidator::read_varint(size_t& offset, uint64_t& out) {
  if (offset >= m_schema.bytecode.size())
    return false;

  auto   buf        = m_schema.bytecode.data() + offset;
  size_t max_len    = m_schema.bytecode.size() - offset;
  size_t bytes_read = decode_varint(buf, max_len, out);
  if (bytes_read == 0)
    return false;
  offset += bytes_read;
  return true;
}

bool SchemaValidator::check_items(
  ERL_NIF_TERM       term,
  size_t             items_schema_offset,
  size_t             start,
  ValidationContext& ctx
) {
  unsigned int list_len = 0;
  if (enif_get_list_length(m_env, term, &list_len) == 0)
    return true;

  // items applies to every position past the prefix, so all of them are evaluated
  if (ctx.m_eval) ctx.m_eval->items = std::numeric_limits<size_t>::max();

  ERL_NIF_TERM head, tail = term;
  size_t index = 0;

  // Report array iteration work to scheduler
  update_reduction_count(m_env, list_len * 10);

  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    if (index < start) {  // covered by prefixItems
      ++index;
      continue;
    }
    if (ctx.m_current_depth >= ctx.m_max_depth) {
      ctx.add_error(ValidationError{
        .reason = ErrorReason::MAX_DEPTH_EXCEEDED
      });
      return false;
    }

    ctx.m_path.push_back(enif_make_ulong(m_env, index));
    ++ctx.m_current_depth;

    bool item_valid = validate_child(head, items_schema_offset, ctx);

    --ctx.m_current_depth;
    ctx.m_path.pop_back();

    if (!item_valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;

    ++index;
    if (ctx.should_stop()) break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_properties(
  ERL_NIF_TERM term,
  const std::vector<std::pair<std::string_view, size_t>>& props,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  for (const auto& [name, offset] : props) {
    ERL_NIF_TERM value;
    if (!get_map_field(term, name, &value))
      continue;  // Absence is reported by `required`, not here

    if (ctx.m_eval) ctx.m_eval->props.insert(std::string(name));
    ctx.m_path.push_back(name_term(name));
    ++ctx.m_current_depth;
    bool valid = validate_child(value, offset, ctx);
    --ctx.m_current_depth;
    ctx.m_path.pop_back();

    if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;
    if (ctx.should_stop())
      break;
  }

  return ctx.m_errors.empty();
}

// Look up a property by name. Keys may be binaries (default decoding) or
// existing atoms (maps built from atom-keyed Erlang terms), so try both.
ERL_NIF_TERM SchemaValidator::name_term(std::string_view name) {
  // A name is cached only when it lies in the bytecode: that outlives the
  // validation, so the cache's pointer to it stays valid
  const char* bc = reinterpret_cast<const char*>(m_schema.bytecode.data());
  bool in_bytecode = name.data() >= bc && name.data() + name.size() <= bc + m_schema.bytecode.size();
  if (!in_bytecode)
    return binary_term(name);
  uint32_t h = KeyCache::hash_of(name.data(), name.size());
  if (ERL_NIF_TERM cached = m_names.lookup(name.data(), name.size(), h))
    return cached;
  ERL_NIF_TERM term = binary_term(name);
  m_names.insert(name.data(), name.size(), h, term);
  return term;
}

bool SchemaValidator::get_map_field(ERL_NIF_TERM map, std::string_view name, ERL_NIF_TERM* out) {
  if (enif_get_map_value(m_env, map, name_term(name), out))
    return true;
  ERL_NIF_TERM atom_key;
  if (enif_make_existing_atom_len(m_env, name.data(), name.size(), &atom_key, ERL_NIF_UTF8))
    return enif_get_map_value(m_env, map, atom_key, out);
  return false;
}

bool SchemaValidator::check_required(
  ERL_NIF_TERM term,
  const std::vector<std::string_view>& required_fields,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  for (const auto& field : required_fields) {
    ERL_NIF_TERM value;
    if (!get_map_field(term, field, &value)) {
      ctx.add_error(ValidationError{
        .reason        = ErrorReason::MISSING_REQUIRED,
        .reason_detail = binary_term(field),
        .actual_value  = term
      });
      if (ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    }
  }

  return ctx.m_errors.empty();
}

// The first pattern that matches or fails (an engine error stops the search);
// NoMatch when none does
inline RegexMatch matches_any_pattern_impl(std::string_view text, const std::vector<size_t>& patterns,
                                           const std::vector<std::shared_ptr<SchemaRegex>>& all) {
  for (size_t idx : patterns) {
    if (idx >= all.size()) continue;
    RegexMatch r = all[idx]->match(text);
    if (r.status != RegexMatch::Status::NoMatch)
      return r;
  }
  return {};
}

bool SchemaValidator::check_additional_properties(
  ERL_NIF_TERM term,
  int mode,
  const std::vector<std::string_view>& declared,
  const std::vector<size_t>& patterns,
  size_t schema_offset,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  auto iter_opt = MapIterator::create(m_env, term);
  if (!iter_opt)
    return true;

  ERL_NIF_TERM key, value;
  std::string scratch;   // holds atom keys only
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string_view name;
    if (!key_view(key, scratch, name))
      continue;
    if (std::binary_search(declared.begin(), declared.end(), name))   // declared is sorted
      continue;
    RegexMatch hit = matches_any_pattern_impl(name, patterns, m_schema.patterns);
    if (hit.status == RegexMatch::Status::Error) {
      ctx.add_error(regex_failure(m_env, term, hit.code));
      return false;
    }
    if (hit.status == RegexMatch::Status::Match)
      continue;

    if (ctx.m_eval) ctx.m_eval->props.insert(std::string(name));
    if (mode == 2)
      continue;   // allowed without constraint, but evaluated
    ERL_NIF_TERM name_term = binary_term(name);
    ctx.m_path.push_back(name_term);

    bool valid = true;
    if (mode == 1) {
      ++ctx.m_current_depth;
      valid = validate_child(value, schema_offset, ctx);
      --ctx.m_current_depth;
    } else {
      ctx.add_error(ValidationError{
        .reason        = ErrorReason::EXTRA_PROPERTIES,
        .reason_detail = name_term,
        .actual_value  = value
      });
      valid = false;
    }

    ctx.m_path.pop_back();

    if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;
    if (ctx.should_stop())
      break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_not(
  ERL_NIF_TERM       term,
  size_t             not_schema_offset,
  ValidationContext& ctx
) {
  ValidationContext temp_ctx{
    .m_env           = m_env,
    .m_mode          = ValidationMode::FAIL_FAST,
    .m_max_depth     = ctx.m_max_depth,
    .m_current_depth = ctx.m_current_depth,
    .m_start_time    = ctx.m_start_time,   // the deadline applies inside not
    .m_timeout_ms    = ctx.m_timeout_ms
  };
  // Annotations inside a not are still collected (and then discarded with the
  // result), so an unevaluated* keyword within it sees its siblings
  Evaluated inner;
  temp_ctx.m_eval = m_schema.track_annotations ? &inner : nullptr;
  temp_ctx.m_dyn_scope = ctx.m_dyn_scope;

  bool matches = validate_value(term, not_schema_offset, temp_ctx);
  ctx.m_timed_out = ctx.m_timed_out || temp_ctx.m_timed_out;

  (void)matches;
  if (temp_ctx.m_errors.empty()) {
    ctx.add_error(ValidationError{
      .reason       = ErrorReason::SCHEMA_MATCHED,
      .actual_value = term
    });
    return false;
  }

  return true;
}

bool SchemaValidator::read_program(size_t& bytecode_offset, size_t& out_offset) {
  uint64_t prog = 0;
  if (!read_varint(bytecode_offset, prog) || prog >= m_schema.program_offsets.size())
    return false;
  out_offset = m_schema.program_offsets[prog];
  return true;
}

void SchemaValidator::merge_evaluated(Evaluated& into, const Evaluated& from) {
  into.props.insert(from.props.begin(), from.props.end());
  into.items = std::max(into.items, from.items);
  into.item_set.insert(from.item_set.begin(), from.item_set.end());
  into.all_props = into.all_props || from.all_props;
}

bool SchemaValidator::validate_applicator(ERL_NIF_TERM term, size_t offset, ValidationContext& ctx) {
  Evaluated own;
  Evaluated* parent = ctx.m_eval;
  ctx.m_eval = m_schema.track_annotations ? &own : nullptr;
  ctx.m_dyn_scope.push_back(offset);   // entering a schema makes its resource part of the dynamic scope
  bool ok = validate_value(term, offset, ctx);
  ctx.m_dyn_scope.pop_back();
  ctx.m_eval = parent;
  if (parent)
    merge_evaluated(*parent, own);
  return ok;
}

bool SchemaValidator::validate_child(ERL_NIF_TERM term, size_t offset, ValidationContext& ctx) {
  Evaluated own;
  Evaluated* saved = ctx.m_eval;
  ctx.m_eval = m_schema.track_annotations ? &own : nullptr;
  bool ok = validate_value(term, offset, ctx);
  ctx.m_eval = saved;
  return ok;
}

bool SchemaValidator::matches(
  ERL_NIF_TERM term,
  size_t offset,
  ValidationContext& ctx,
  Evaluated* annotations
) {
  // A probe context: fail-fast, errors dropped, same depth, deadline and scope
  ValidationContext probe{
    .m_env           = m_env,
    .m_mode          = ValidationMode::FAIL_FAST,
    .m_max_errors    = 1,
    .m_max_depth     = ctx.m_max_depth,
    .m_current_depth = ctx.m_current_depth,
    .m_start_time    = ctx.m_start_time,
    .m_timeout_ms    = ctx.m_timeout_ms
  };
  Evaluated own;
  probe.m_eval = m_schema.track_annotations ? &own : nullptr;
  probe.m_dyn_scope = ctx.m_dyn_scope;

  probe.m_dyn_scope.push_back(offset);
  validate_value(term, offset, probe);
  ctx.m_timed_out = ctx.m_timed_out || probe.m_timed_out;

  bool ok = probe.m_errors.empty();
  if (ok && annotations)
    merge_evaluated(*annotations, own);
  return ok;
}

bool SchemaValidator::check_unevaluated_properties(
  ERL_NIF_TERM term,
  bool forbid,
  size_t schema_offset,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;
  if (ctx.m_eval && ctx.m_eval->all_props)
    return true;

  auto iter_opt = MapIterator::create(m_env, term);
  if (!iter_opt)
    return true;

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name))
      continue;
    if (ctx.m_eval && ctx.m_eval->props.count(name))
      continue;

    ERL_NIF_TERM name_term = binary_term(name);
    if (forbid) {
      ctx.add_error(ValidationError{
        .reason        = ErrorReason::EXTRA_PROPERTIES,
        .reason_detail = name_term,
        .actual_value  = value
      });
      if (ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    } else {
      ctx.m_path.push_back(name_term);
      ++ctx.m_current_depth;
      bool valid = validate_child(value, schema_offset, ctx);
      --ctx.m_current_depth;
      ctx.m_path.pop_back();
      if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    }
    if (ctx.should_stop())
      break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_unevaluated_items(
  ERL_NIF_TERM term,
  bool forbid,
  size_t schema_offset,
  ValidationContext& ctx
) {
  if (!enif_is_list(m_env, term))
    return true;

  size_t covered = ctx.m_eval ? ctx.m_eval->items : 0;
  ERL_NIF_TERM head, tail = term;
  size_t index = 0;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    if (index < covered || (ctx.m_eval && ctx.m_eval->item_set.count(index))) {
      ++index;
      continue;
    }

    if (forbid) {
      ctx.add_error(ValidationError{
        .reason        = ErrorReason::TOO_MANY_ITEMS,
        .reason_detail = enif_make_ulong(m_env, index),
        .actual_value  = head
      });
      if (ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    } else {
      ctx.m_path.push_back(enif_make_ulong(m_env, index));
      ++ctx.m_current_depth;
      bool valid = validate_child(head, schema_offset, ctx);
      --ctx.m_current_depth;
      ctx.m_path.pop_back();
      if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    }

    if (ctx.should_stop())
      break;
    ++index;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_any_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  // Every branch is tried, not just the first match: each matching branch
  // contributes its annotations
  bool any = false;
  for (size_t off : schemas) {
    if (matches(term, off, ctx, ctx.m_eval))
      any = true;
  }
  if (any)
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::NO_SCHEMA_MATCHED,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_one_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  size_t count = 0;
  for (size_t off : schemas) {
    if (matches(term, off, ctx, ctx.m_eval))
      ++count;
  }

  if (count == 1)
    return true;

  if (count == 0) {
    ctx.add_error(ValidationError{
      .reason       = ErrorReason::NO_SCHEMA_MATCHED,
      .actual_value = term
    });
    return false;
  }

  ctx.add_error(ValidationError{
    .reason        = ErrorReason::MULTIPLE_SCHEMAS_MATCHED,
    .reason_detail = enif_make_uint64(m_env, count),
    .actual_value  = term
  });
  return false;
}

bool SchemaValidator::check_all_of(
  ERL_NIF_TERM term,
  const std::vector<size_t>& schemas,
  ValidationContext& ctx
) {
  // Run each schema against the real context so its own errors are reported
  for (size_t off : schemas) {
    validate_applicator(term, off, ctx);
    if (ctx.m_mode == ValidationMode::FAIL_FAST && !ctx.m_errors.empty())
      return false;
    if (ctx.should_stop())
      break;
  }
  return ctx.m_errors.empty();
}

bool SchemaValidator::check_if_then_else(
  ERL_NIF_TERM term,
  size_t if_off,
  size_t then_off,
  size_t else_off,
  ValidationContext& ctx
) {
  size_t branch = matches(term, if_off, ctx, ctx.m_eval) ? then_off : else_off;
  if (branch == NO_BRANCH)
    return ctx.m_errors.empty();

  validate_applicator(term, branch, ctx);
  return ctx.m_errors.empty();
}

bool SchemaValidator::check_prefix_items(
  ERL_NIF_TERM term,
  const std::vector<size_t>& offsets,
  ValidationContext& ctx
) {
  if (!enif_is_list(m_env, term))
    return true;

  ERL_NIF_TERM head, tail = term;
  size_t index = 0;
  while (index < offsets.size() && enif_get_list_cell(m_env, tail, &head, &tail)) {
    ctx.m_path.push_back(enif_make_ulong(m_env, index));
    ++ctx.m_current_depth;
    bool valid = validate_child(head, offsets[index], ctx);
    --ctx.m_current_depth;
    ctx.m_path.pop_back();

    if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;
    if (ctx.should_stop())
      break;
    ++index;
  }

  if (ctx.m_eval) ctx.m_eval->items = std::max(ctx.m_eval->items, index);
  return ctx.m_errors.empty();
}

bool SchemaValidator::check_dependent_required(
  ERL_NIF_TERM term,
  const std::vector<std::pair<std::string, std::vector<std::string>>>& deps,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  for (const auto& [name, required] : deps) {
    ERL_NIF_TERM ignored;
    if (!get_map_field(term, name, &ignored))
      continue;

    for (const auto& field : required) {
      if (get_map_field(term, field, &ignored))
        continue;

      ctx.add_error(ValidationError{
        .reason        = ErrorReason::DEPENDENT_KEYS_MISSING,
        .reason_detail = binary_term(field),
        .actual_value  = term
      });
      if (ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
    }

    if (ctx.should_stop())
      break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::check_dependent_schemas(
  ERL_NIF_TERM term,
  const std::vector<std::pair<std::string, size_t>>& deps,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  for (const auto& [name, offset] : deps) {
    ERL_NIF_TERM ignored;
    if (!get_map_field(term, name, &ignored))
      continue;

    ++ctx.m_current_depth;
    bool valid = validate_applicator(term, offset, ctx);
    --ctx.m_current_depth;

    if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;
    if (ctx.should_stop())
      break;
  }

  return ctx.m_errors.empty();
}

bool SchemaValidator::read_varint_at(const uint8_t* buf, size_t len, size_t& pos, uint64_t& out) {
  if (pos >= len)
    return false;
  size_t used = decode_varint(buf + pos, len - pos, out);
  if (used == 0)
    return false;
  pos += used;
  return true;
}

bool SchemaValidator::equal_encoded(ERL_NIF_TERM term, const uint8_t* buf, size_t len, size_t& pos) {
  if (pos >= len)
    return false;
  uint8_t tag = buf[pos++];
  switch (tag) {
    case 'T':
      return enif_is_identical(term, AM_TRUE);
    case 'F':
      return enif_is_identical(term, AM_FALSE);
    case 'N':   // null: any atom that is not a boolean
      return enif_is_atom(m_env, term) && !enif_is_identical(term, AM_TRUE) && !enif_is_identical(term, AM_FALSE);
    case 'i': {
      if (len - pos < sizeof(int64_t)) return false;
      int64_t v = 0;
      std::memcpy(&v, buf + pos, sizeof(v));
      pos += sizeof(v);
      ErlNifSInt64 t = 0;
      if (enif_get_int64(m_env, term, &t))
        return static_cast<int64_t>(t) == v;
      BigNum tb;
      if (BigNum::from_term(m_env, term, tb))
        return BigNum::compare(tb, BigNum::from_int64(v)) == 0;
      double dt = 0;
      return enif_get_double(m_env, term, &dt) && BigNum::compare_double(BigNum::from_int64(v), dt) == 0;
    }
    case 'b': {   // integer beyond int64: sign byte, varint length, magnitude bytes
      if (pos >= len) return false;
      bool negative = buf[pos++] != 0;
      uint64_t n = 0;
      if (!read_varint_at(buf, len, pos, n) || n > len - pos) return false;
      BigNum enc;
      enc.mag.assign((n + 3) / 4, 0u);
      for (size_t i = 0; i < n; ++i)
        enc.mag[i / 4] |= uint32_t(buf[pos + i]) << (8 * (i % 4));
      enc.negative = negative;
      enc.trim();
      pos += n;
      BigNum tb;
      if (BigNum::from_term(m_env, term, tb))
        return BigNum::compare(tb, enc) == 0;
      double dt = 0;
      return enif_get_double(m_env, term, &dt) && BigNum::compare_double(enc, dt) == 0;
    }
    case 'd': {
      if (len - pos < sizeof(double)) return false;
      double v = 0;
      std::memcpy(&v, buf + pos, sizeof(v));
      pos += sizeof(v);
      BigNum tb;
      if (BigNum::from_term(m_env, term, tb))
        return BigNum::compare_double(tb, v) == 0;
      double dt = 0;
      return enif_get_double(m_env, term, &dt) && dt == v;
    }
    case 's': {
      uint64_t n = 0;
      if (!read_varint_at(buf, len, pos, n) || n > len - pos) return false;
      ErlNifBinary bin;
      if (!enif_inspect_binary(m_env, term, &bin) || bin.size != n) return false;
      bool eq = (n == 0) || std::memcmp(bin.data, buf + pos, n) == 0;
      pos += n;
      return eq;
    }
    case 'a': {
      uint64_t count = 0;
      if (!read_varint_at(buf, len, pos, count)) return false;
      unsigned int elen = 0;
      if (!enif_is_list(m_env, term) || !enif_get_list_length(m_env, term, &elen) || elen != count)
        return false;
      ERL_NIF_TERM head, tail = term;
      while (enif_get_list_cell(m_env, tail, &head, &tail)) {
        if (!equal_encoded(head, buf, len, pos)) return false;
      }
      return true;
    }
    case 'o': {
      uint64_t count = 0;
      if (!read_varint_at(buf, len, pos, count)) return false;
      size_t msize = 0;
      if (!enif_is_map(m_env, term) || !enif_get_map_size(m_env, term, &msize) || msize != count)
        return false;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t klen = 0;
        if (!read_varint_at(buf, len, pos, klen) || klen > len - pos) return false;
        std::string key(reinterpret_cast<const char*>(buf + pos), klen);
        pos += klen;
        ERL_NIF_TERM value;
        if (!get_map_field(term, key, &value)) return false;
        if (!equal_encoded(value, buf, len, pos)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

bool SchemaValidator::terms_equal(ERL_NIF_TERM a, ERL_NIF_TERM b) {
  if (enif_is_identical(a, b))
    return true;

  ErlNifSInt64 la = 0, lb = 0;
  if (enif_get_int64(m_env, a, &la) && enif_get_int64(m_env, b, &lb))
    return la == lb;

  // Numbers: integers exactly (of any size), floats as doubles, and an integer
  // against a float by an exact comparison
  BigNum na_big, nb_big;
  bool ia = BigNum::from_term(m_env, a, na_big);
  bool ib = BigNum::from_term(m_env, b, nb_big);
  double da = 0, db = 0;
  bool fa = !ia && enif_get_double(m_env, a, &da);
  bool fb = !ib && enif_get_double(m_env, b, &db);
  if (ia && ib) return BigNum::compare(na_big, nb_big) == 0;
  if (ia && fb) return BigNum::compare_double(na_big, db) == 0;
  if (fa && ib) return BigNum::compare_double(nb_big, da) == 0;
  if (fa && fb) return da == db;
  if (ia || ib || fa || fb) return false;   // a number against a non-number

  ErlNifBinary ba, bb;
  if (enif_inspect_binary(m_env, a, &ba) && enif_inspect_binary(m_env, b, &bb))
    return ba.size == bb.size && (ba.size == 0 || std::memcmp(ba.data, bb.data, ba.size) == 0);

  if (enif_is_list(m_env, a) && enif_is_list(m_env, b)) {
    unsigned int la_len = 0, lb_len = 0;
    if (!enif_get_list_length(m_env, a, &la_len) || !enif_get_list_length(m_env, b, &lb_len) || la_len != lb_len)
      return false;
    ERL_NIF_TERM ha, ta = a, hb, tb = b;
    while (enif_get_list_cell(m_env, ta, &ha, &ta) && enif_get_list_cell(m_env, tb, &hb, &tb)) {
      if (!terms_equal(ha, hb)) return false;
    }
    return true;
  }

  if (enif_is_map(m_env, a) && enif_is_map(m_env, b)) {
    size_t sa = 0, sb = 0;
    if (!enif_get_map_size(m_env, a, &sa) || !enif_get_map_size(m_env, b, &sb) || sa != sb)
      return false;
    auto iter_opt = MapIterator::create(m_env, a);
    if (!iter_opt) return false;
    ERL_NIF_TERM key, value;
    for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
      std::string name;
      if (!get_string(key, name)) return false;
      ERL_NIF_TERM other;
      if (!get_map_field(b, name, &other) || !terms_equal(value, other)) return false;
    }
    return true;
  }

  // Atoms (booleans, null) are equal only when identical; handled above
  return false;
}

bool SchemaValidator::check_const(ERL_NIF_TERM term, size_t offset, size_t length, ValidationContext& ctx) {
  size_t pos = 0;
  const uint8_t* buf = m_schema.bytecode.data() + offset;
  if (equal_encoded(term, buf, length, pos))
    return true;

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::NOT_CONST,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_enum(
  ERL_NIF_TERM term,
  const std::vector<std::pair<size_t, size_t>>& values,
  ValidationContext& ctx
) {
  for (const auto& [offset, length] : values) {
    size_t pos = 0;
    if (equal_encoded(term, m_schema.bytecode.data() + offset, length, pos))
      return true;
  }

  ctx.add_error(ValidationError{
    .reason       = ErrorReason::NOT_IN_ENUM,
    .actual_value = term
  });
  return false;
}

bool SchemaValidator::check_contains(
  ERL_NIF_TERM term,
  size_t schema_offset,
  size_t min_count,
  size_t max_count,
  bool has_max,
  ValidationContext& ctx
) {
  if (!enif_is_list(m_env, term))
    return true;

  size_t count = 0;
  size_t index = 0;
  ERL_NIF_TERM head, tail = term;
  while (enif_get_list_cell(m_env, tail, &head, &tail)) {
    if (matches(head, schema_offset, ctx, nullptr)) {
      ++count;
      if (ctx.m_eval) ctx.m_eval->item_set.insert(index);
    }
    ++index;
  }

  if (count < min_count) {
    ctx.add_error(ValidationError{
      .reason        = ErrorReason::CONTAINS_FAILED,
      .reason_detail = enif_make_uint64(m_env, count),
      .actual_value  = term
    });
    return false;
  }
  if (has_max && count > max_count) {
    ctx.add_error(ValidationError{
      .reason        = ErrorReason::TOO_MANY_ITEMS,
      .reason_detail = enif_make_uint64(m_env, count),
      .actual_value  = term
    });
    return false;
  }
  return true;
}

bool SchemaValidator::check_pattern_properties(
  ERL_NIF_TERM term,
  const std::vector<std::pair<size_t, size_t>>& entries,
  ValidationContext& ctx
) {
  if (!enif_is_map(m_env, term))
    return true;

  auto iter_opt = MapIterator::create(m_env, term);
  if (!iter_opt)
    return true;

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name))
      continue;
    for (const auto& [pidx, offset] : entries) {
      if (pidx >= m_schema.patterns.size())
        continue;
      RegexMatch hit = m_schema.patterns[pidx]->match(name);
      if (hit.status == RegexMatch::Status::Error) {
        ctx.add_error(regex_failure(m_env, term, hit.code));
        return false;
      }
      if (hit.status != RegexMatch::Status::Match)
        continue;

      if (ctx.m_eval) ctx.m_eval->props.insert(name);
      ERL_NIF_TERM name_term = binary_term(name);
      ctx.m_path.push_back(name_term);
      ++ctx.m_current_depth;
      bool valid = validate_child(value, offset, ctx);
      --ctx.m_current_depth;
      ctx.m_path.pop_back();

      if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
        return false;
      if (ctx.should_stop())
        return ctx.m_errors.empty();
    }
  }
  return ctx.m_errors.empty();
}

bool SchemaValidator::check_property_names(ERL_NIF_TERM term, size_t schema_offset, ValidationContext& ctx) {
  if (!enif_is_map(m_env, term))
    return true;

  auto iter_opt = MapIterator::create(m_env, term);
  if (!iter_opt)
    return true;

  ERL_NIF_TERM key, value;
  for (auto& iter = iter_opt.value(); iter.get_pair(&key, &value); iter.next()) {
    std::string name;
    if (!get_string(key, name))
      continue;

    ERL_NIF_TERM name_term = binary_term(name);
    ctx.m_path.push_back(name_term);
    ++ctx.m_current_depth;
    bool valid = validate_child(name_term, schema_offset, ctx);
    --ctx.m_current_depth;
    ctx.m_path.pop_back();

    if (!valid && ctx.m_mode == ValidationMode::FAIL_FAST)
      return false;
    if (ctx.should_stop())
      break;
  }
  return ctx.m_errors.empty();
}

// Format assertion (only compiled in when the format_assertion option is set).
// Non-string instances and unknown format names are not checked.
bool check_format_value(ErlNifEnv* env, ERL_NIF_TERM term, std::string_view format) {
  ErlNifBinary bin;
  if (!enif_inspect_binary(env, term, &bin))
    return true;
  std::string_view text(reinterpret_cast<const char*>(bin.data), bin.size);
  if (format == "regex") {
    // A regex format value is valid when it compiles
    try {
      SchemaRegex compiled{std::string(text)};
      return true;
    } catch (const SchemaRegexError&) {
      return false;
    }
  }
  bool known = false;
  return formats::check(format, text, known);
}

} // namespace glz