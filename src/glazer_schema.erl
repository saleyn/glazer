-module(glazer_schema).
-moduledoc """
Schemas for JSON and YAML documents: compile, validate and register them.

- `compile/1,2` compiles a schema map to a handle, and
  `compile_file/1,2` reads a `.json`, `.yaml` or `.yml` file and compiles
  it (the format comes from the extension, or from the `format` option).
- `validate/2,3` checks a term against a handle or a registered name (a file
  name is not a registered name: load the file with `compile_file/1,2` first), and
  `validate_term/2,3` compiles and validates in one call.
- `register/1,2,3` keeps compiled schemas under names, in a process-free registry
  (`persistent_term`). A schema with a top-level `$id` is named by it.

The data being validated is any Erlang term, so a document decoded with
`glazer_json` or `glazer_yaml` can be validated directly.

## Example

```erlang
%% schemas/user.yaml has "$id: https://example.com/user.json", so it is registered under that name
{ok, _} = glazer_schema:compile_file("schemas/user.yaml"),
ok = glazer_schema:validate(<<"https://example.com/user.json">>, #{<<"name">> => <<"Ann">>}),
{ok, 1} = glazer_schema:register(#{<<"$id">> => <<"order">>, <<"type">> => <<"object">>}).
```
""".

-export([register/1, register/2, register/3, register_group/1, register_group/2, unregister/1,
         fetch/1, 'fetch!'/1, version/1, list/0,
         validate/2, validate/3, validate_term/2, validate_term/3,
         name_of/1, register_handle/3, resolve_refs/2,
         intern/2, clear_interned/0,
         compile/1, compile/2, compile_file/1, compile_file/2]).

-export_type([name/0]).

-type name() :: term().
-type compiled() :: {schema, reference()} | reference().

-define(INDEX, {?MODULE, index}).
-define(ENTRY(Name), {?MODULE, schema, Name}).
-define(LOCK, {?MODULE, lock}).
-define(INTERNED(Key), {?MODULE, interned, Key}).

-doc """
Register `Schema` under the name in its top-level `$id`. Returns
`{error, Message}` when the schema has no `$id`. See `register/3`.
""".
-spec register(map()) -> {ok, pos_integer()} | {error, binary()}.
register(Schema) ->
  case name_of(Schema) of
    {ok, Name} -> register(Name, Schema, []);
    error      -> {error, ~"schema has no $id to use as its name"}
  end.

-doc """
Compile `Schema` and store it under `Name`. Equivalent to `register/3` with
no compile options.
""".
-spec register(name(), map() | boolean()) -> {ok, pos_integer()} | {error, binary()}.
register(Name, Schema) ->
  register(Name, Schema, []).

-doc """
Compile `Schema` with `CompileOpts` (see `glazer_schema:compile/2`) and
store it under `Name`.

Returns `{ok, Version}`:

- If `Name` already holds the same schema and options, nothing is recompiled
  and the current version is returned.
- Otherwise the schema is compiled and stored under a new version (versions
  start at 1 and increase by 1 on each change of the same name).

On a compile error nothing is stored, and `{error, Message}` is returned with
the message from the compiler, as in `glazer_schema:compile/1`.
""".
-spec register(name(), map() | boolean(), list()) -> {ok, pos_integer()} | {error, binary()}.
register(Name, Schema, CompileOpts) ->
  case register_handle(Name, Schema, CompileOpts) of
    {ok, Version, _Compiled} -> {ok, Version};
    {error, _} = Error       -> Error
  end.

-doc """
As `register/3`, but returns the compiled handle as well:
`{ok, Version, {schema, Resource}}`.
""".
-spec register_handle(name(), map() | boolean(), list()) ->
        {ok, pos_integer(), {schema, reference()}} | {error, binary()}.
register_handle(Name, Schema, CompileOpts) ->
  global:trans({?LOCK, ?MODULE}, fun() -> do_register(Name, Schema, CompileOpts) end).

do_register(Name, Schema, CompileOpts) ->
  case prepare(Name, Schema, CompileOpts) of
    {unchanged, Version, Compiled} ->
      {ok, Version, Compiled};
    {error, _} = Error ->
      Error;
    {new, Entry} ->
      store(Name, Entry),
      {ok, maps:get(version, Entry), maps:get(schema, Entry)}
  end.

%% Compile Schema under Name without storing it. unchanged when the stored
%% entry already has this schema and options.
prepare(Name, Schema, CompileOpts0) ->
  Opts = resolve_refs(Schema, CompileOpts0),
  case persistent_term:get(?ENTRY(Name), undefined) of
    #{source := Schema, opts := Opts, version := Version, schema := Compiled} ->
      {unchanged, Version, Compiled};
    Previous ->
      case nif_compile(Schema, Opts) of
        {ok, Compiled} ->
          Version = case Previous of
            #{version := V} -> V + 1;
            undefined       -> 1
          end,
          {new, #{name => Name, schema => Compiled, source => Schema,
                  opts => Opts, version => Version}};
        {error, Message} ->
          {error, Message}
      end
  end.

store(Name, Entry) ->
  persistent_term:put(?ENTRY(Name), maps:without([name], Entry)),
  set_index(lists:usort([Name | names()])).

-doc """
Register a group of schemas that refer to each other, such as `A` with a
`$ref` to `B` and `B` with a `$ref` to `A`. Each schema is compiled with the
others in the group as its documents, so every reference resolves. Nothing is
stored unless every schema compiles.

Every schema needs a `$id`, which is its name. Returns `{ok, [{Name, Version}]}`
in the order given, or `{error, Message}` with nothing stored.

Compiling a schema later with a changed version of a group member does not
recompile the others. Register the group again to pick up such changes.
""".
-spec register_group([map()], list()) -> {ok, [{binary(), pos_integer()}]} | {error, binary()}.
-spec register_group([map()]) -> {ok, [{binary(), pos_integer()}]} | {error, binary()}.
register_group(Schemas) ->
  register_group(Schemas, []).

register_group(Schemas, CompileOpts) ->
  Named = [case name_of(S) of
             {ok, N} -> {N, S};
             error   -> {error, ~"every schema in a group needs a $id"}
           end || S <- Schemas],
  case [Error || {error, _} = Error <- Named] of
    [Error | _] ->
      Error;
    [] ->
      Names = [N || {N, _} <- Named],
      case length(lists:usort(Names)) =:= length(Names) of
        false -> {error, ~"duplicate $id in group"};
        true  -> global:trans({?LOCK, ?MODULE}, fun() -> group_register(Named, CompileOpts) end)
      end
  end.

group_register(Named, CompileOpts) ->
  Sources = maps:from_list(Named),
  Prepared = [{N, prepare(N, S, [{refs, maps:remove(N, Sources)} | CompileOpts])}
              || {N, S} <- Named],
  case [Error || {_, {error, _} = Error} <- Prepared] of
    [Error | _] ->
      Error;
    [] ->
      Results = [{N, case P of
                       {unchanged, V, _} -> V;
                       {new, Entry}      -> store(N, Entry), maps:get(version, Entry)
                     end}
                 || {N, P} <- Prepared],
      {ok, Results}
  end.

-doc """
Remove `Name` from the registry. Its compiled schema is released once no
caller holds a reference to it. Unknown names are ignored.
""".
-spec unregister(name()) -> ok.
unregister(Name) ->
  global:trans({?LOCK, ?MODULE}, fun() ->
    case persistent_term:get(?ENTRY(Name), undefined) of
      undefined -> ok;
      _ ->
        persistent_term:erase(?ENTRY(Name)),
        set_index([N || N <- names(), N =/= Name]),
        ok
    end
  end).

-doc """
Fetch the compiled schema registered under `Name`, as `{schema, Resource}`
(the form `glazer_schema:validate/2` takes). This is a single lookup and copies
no schema data.
""".
-spec fetch(name()) -> {ok, {schema, reference()}} | {error, not_found}.
fetch(Name) ->
  case persistent_term:get(?ENTRY(Name), undefined) of
    #{schema := Compiled} -> {ok, Compiled};
    undefined             -> {error, not_found}
  end.

-doc """
Like `fetch/1`, but raises `{unknown_schema, Name}` when `Name` is not
registered.
""".
-spec 'fetch!'(name()) -> {schema, reference()}.
'fetch!'(Name) ->
  case fetch(Name) of
    {ok, Compiled} -> Compiled;
    {error, not_found} -> error({unknown_schema, Name})
  end.

-doc "The current version of `Name`, or `{error, not_found}`.".
-spec version(name()) -> {ok, pos_integer()} | {error, not_found}.
version(Name) ->
  case persistent_term:get(?ENTRY(Name), undefined) of
    #{version := V} -> {ok, V};
    undefined       -> {error, not_found}
  end.

-doc """
All registered names with their current versions, sorted by name.
""".
-spec list() -> [{name(), pos_integer()}].
list() ->
  [{Name, V} || Name <- names(), {ok, V} <- [version(Name)]].

-doc """
The name a schema is registered under: its top-level `$id` without any
fragment. `error` when the schema is not a map, has no `$id`, or its `$id`
is only a fragment such as `#item`.
""".
-spec name_of(term()) -> {ok, binary()} | error.
name_of(#{~"$id" := Id}) when is_binary(Id) ->
  case doc_part(Id) of
    ~"" -> error;
    Name -> {ok, Name}
  end;
name_of(_) ->
  error.

-doc """
Add the registered documents that `Schema` refers to, to the `refs` option.

Each `$ref` that points at another document (not `#...` within the schema) is
looked up in the registry, and the documents those refer to are added in turn.
The result is `Options` with a `{refs, Map}` entry holding everything found.
An entry in `Options` with the same URI takes precedence over the registry.
Unknown URIs are left out, so the compiler reports them as unresolved.
""".
-spec resolve_refs(term(), list()) -> list().
resolve_refs(Schema, Options) ->
  {Explicit, Rest} = case lists:keytake(refs, 1, Options) of
    {value, {refs, Map}, Others} -> {Map, Others};
    false                        -> {#{}, Options}
  end,
  Own = case name_of(Schema) of
    {ok, Name} -> [Name];
    error      -> []
  end,
  Found = closure(refs_in(Schema), Own, Explicit, #{}),
  case maps:size(Found) + maps:size(Explicit) of
    0 -> Rest;
    _ -> [{refs, maps:merge(Found, Explicit)} | Rest]
  end.

-doc """
Compile `Schema` with `Opts` and return the stored handle, if one exists for
the same schema and options; otherwise compile it and store it. This is what
`glazer_schema:compile/2` uses for schemas without a `$id`. Interned
schemas are keyed by their content and are not part of the name index, so
`list/0` does not show them.

Entries live until `clear_interned/0` is called. Each new entry triggers a
global garbage collection, so avoid interning on every request for schemas
that change per request; pass `{registry, false}` for those.
""".
-spec intern(map() | boolean(), list()) -> {ok, {schema, reference()}} | {error, binary()}.
intern(Schema, Opts0) ->
  Opts = resolve_refs(Schema, Opts0),
  Key = ?INTERNED({Schema, Opts}),
  case persistent_term:get(Key, undefined) of
    undefined ->
      case nif_compile(Schema, Opts) of
        {ok, Compiled} ->
          persistent_term:put(Key, Compiled),
          {ok, Compiled};
        {error, _} = Error ->
          Error
      end;
    Compiled ->
      {ok, Compiled}
  end.

-doc """
Remove every schema stored by `intern/2` (the default `glazer_schema:compile`
path). Named registrations are not affected. Returns the number removed.
""".
-spec clear_interned() -> non_neg_integer().
clear_interned() ->
  Interned = [K || {K = {?MODULE, interned, _}, _} <- persistent_term:get()],
  [persistent_term:erase(K) || K <- Interned],
  length(Interned).

%% Internal

nif_compile(Schema, []) ->
  glazer:compile_schema(Schema);
nif_compile(Schema, Opts) ->
  glazer:compile_schema(Schema, Opts).

%% The document part of a URI reference: everything before the first '#'
doc_part(Ref) ->
  [Doc | _] = binary:split(Ref, ~"#"),
  Doc.

%% Every document named by a $ref in Term (with duplicates removed)
refs_in(Term) ->
  lists:usort(collect_refs(Term, [])).

collect_refs(Map, Acc0) when is_map(Map) ->
  Acc = case maps:find(~"$ref", Map) of
    {ok, Ref} when is_binary(Ref) -> [doc_part(Ref) | Acc0];
    _ -> Acc0
  end,
  maps:fold(fun(_K, V, A) -> collect_refs(V, A) end, Acc, Map);
collect_refs(List, Acc) when is_list(List) ->
  lists:foldl(fun collect_refs/2, Acc, List);
collect_refs(_, Acc) ->
  Acc.

%% Registered documents reachable from Pending, skipping the schema's own
%% document, explicit refs, empty (local) references and documents already found
closure([], _Own, _Explicit, Acc) ->
  Acc;
closure([Doc | Rest], Own, Explicit, Acc) ->
  Skip = Doc =:= ~"" orelse lists:member(Doc, Own)
         orelse is_map_key(Doc, Explicit) orelse is_map_key(Doc, Acc),
  case Skip of
    true ->
      closure(Rest, Own, Explicit, Acc);
    false ->
      case persistent_term:get(?ENTRY(Doc), undefined) of
        #{source := Source} ->
          closure(refs_in(Source) ++ Rest, Own, Explicit, Acc#{Doc => Source});
        undefined ->
          closure(Rest, Own, Explicit, Acc)
      end
  end.

names() ->
  persistent_term:get(?INDEX, []).

set_index(Names) ->
  persistent_term:put(?INDEX, Names).

-doc """
Compile a JSON Schema. Returns `{ok, {schema, Resource}}` or `{error, Message}`.

By default the compiled schema is added to the schema registry
(`glazer_schema`):

- A schema with a top-level `$id` is registered under that `$id` (without any
  fragment), so it can be fetched and validated by name, and other schemas can
  `$ref` it.
- A schema without `$id` is stored by content: compiling the same schema with
  the same options again returns the stored handle without recompiling.

Compiling the same schema with the same options again returns the stored handle
in both cases. Pass `{registry, false}` to compile without registering.

`$ref`s to other documents are resolved from the registry when they are not
given in `refs` (see `resolve_refs/2`).

Options:

- `{registry, boolean()}`: add to the registry (default `true`).
- `{name, binary() | string()}`: the name to register under. Used when the
  schema has no `$id`. If the schema has a `$id`, the name must equal it
  (without any fragment):
  otherwise the call returns `{error, ...}`, because two names for one schema
  would split its identity. The name is checked this way even when
  `{registry, false}`, but is not registered.
- `{refs, #{Uri => Schema}}`: documents available to `$ref`; takes precedence
  over the registry.
- `{format_assertion, boolean()}`: enforce `format` (default `true`).
""".
-spec compile(map() | boolean()) -> {ok, {schema, reference()}} | {error, binary()}.
compile(Schema) ->
  compile(Schema, []).

-spec compile(map() | boolean(), list()) -> {ok, {schema, reference()}} | {error, binary()}.
compile(Schema, Options) ->
  case {split_name_option(Options), split_registry_option(Options)} of
    {{error, _} = Error, _} ->
      Error;
    {_, {error, _} = Error} ->
      Error;
    {{ok, Given, _}, {ok, false, _}} ->
      %% the name is not registered, but it must still agree with the $id
      case schema_name(Schema, Given) of
        {error, _} = Error ->
          Error;
        _ ->
          Opts = strip_registry(strip_name(Options)),
          compile_direct(Schema, resolve_refs(Schema, Opts))
      end;
    {{ok, Given, _}, {ok, true, _}} ->
      Opts = strip_registry(strip_name(Options)),
      case schema_name(Schema, Given) of
        {ok, Name} ->
          case register_handle(Name, Schema, Opts) of
            {ok, _Version, Compiled} -> {ok, Compiled};
            {error, _} = Error       -> Error
          end;
        none ->
          intern(Schema, Opts);
        {error, _} = Error ->
          Error
      end
  end.

strip_name(Options) ->
  lists:keydelete(name, 1, Options).

strip_registry(Options) ->
  lists:keydelete(registry, 1, Options).

%% The name option is handled here; the NIF does not know it.
split_name_option(Options) ->
  case lists:keytake(name, 1, Options) of
    false ->
      {ok, undefined, Options};
    {value, {name, Name}, Rest} ->
      case normalize_name(Name) of
        {ok, Bin} -> {ok, Bin, Rest};
        error     -> {error, <<"name must be a non-empty binary or string">>}
      end
  end.

normalize_name(Name) when is_binary(Name), Name =/= <<>> ->
  {ok, Name};
normalize_name(Name) when is_list(Name), Name =/= [] ->
  case unicode:characters_to_binary(Name) of
    Bin when is_binary(Bin), Bin =/= <<>> -> {ok, Bin};
    _ -> error
  end;
normalize_name(_) ->
  error.

%% The name a schema is registered under. A name option and a $id must agree
%% when both are given: two names for one schema would split its identity
%% (validate/fetch by name and $ref by $id would find different entries).
schema_name(Schema, Given) ->
  case {name_of(Schema), Given} of
    {{ok, Id}, undefined} -> {ok, Id};
    {{ok, Id}, Id}        -> {ok, Id};
    {{ok, _}, _}          -> {error, <<"name option differs from the schema's $id">>};
    {error, undefined}    -> none;
    {error, Name}         -> {ok, Name}
  end.

-doc """
Read a schema from `Filename` and compile it, as `compile/1`.

The format comes from the file extension: `.json` is decoded as JSON, and
`.yaml` or `.yml` as YAML. Any other extension is an error, unless the `format`
option (`json` or `yaml`) says which to use. The `format` option is not passed
to the compiler.

Returns `{ok, {schema, Resource}}`, or one of:

- `{error, {unsupported_format, Extension}}`: the extension is not recognised
  and no `format` option was given.
- `{error, {read_failed, Filename, Reason}}`: the file cannot be read.
- `{error, {invalid_json, Filename, Message}}`: the contents are not JSON.
- `{error, {invalid_yaml, Filename, Message}}`: the contents are not YAML.
- `{error, Message}`: the schema does not compile, as for `compile/1`.

Other options are those of `compile/2`.
""".
-spec compile_file(file:name_all()) ->
        {ok, {schema, reference()}} | {error, term()}.
compile_file(Filename) ->
  compile_file(Filename, []).

-spec compile_file(file:name_all(), list()) ->
        {ok, {schema, reference()}} | {error, term()}.
compile_file(Filename, Options0) ->
  case schema_format(Filename, Options0) of
    {error, _} = Error ->
      Error;
    {ok, Format, Options} ->
      case file:read_file(Filename) of
        {error, Reason} ->
          {error, {read_failed, Filename, Reason}};
        {ok, Bin} ->
          case decode_schema(Format, Bin) of
            {error, Message} ->
              {error, {invalid_tag(Format), Filename, Message}};
            {ok, Schema} ->
              compile(Schema, Options)
          end
      end
  end.

%% The format of a schema file: the `format` option, else the extension
schema_format(Filename, Options) ->
  case lists:keytake(format, 1, Options) of
    {value, {format, Format}, Rest} when Format =:= json; Format =:= yaml ->
      {ok, Format, Rest};
    {value, {format, Other}, _} ->
      {error, {unsupported_format, Other}};
    false ->
      Extension = string:lowercase(unicode:characters_to_binary(filename:extension(Filename))),
      case Extension of
        <<".json">>       -> {ok, json, Options};
        <<".yaml">>       -> {ok, yaml, Options};
        <<".yml">>        -> {ok, yaml, Options};
        _                 -> {error, {unsupported_format, Extension}}
      end
  end.

decode_schema(json, Bin) -> glazer_json:try_decode(Bin);
decode_schema(yaml, Bin) -> glazer_yaml:try_decode(Bin).

invalid_tag(json) -> invalid_json;
invalid_tag(yaml) -> invalid_yaml.


%% The registry option is handled here; the NIF does not know it.
split_registry_option(Options) ->
  case lists:keytake(registry, 1, Options) of
    false ->
      {ok, true, Options};
    {value, {registry, Flag}, Rest} when is_boolean(Flag) ->
      {ok, Flag, Rest};
    {value, {registry, _}, _} ->
      {error, <<"registry must be true or false">>}
  end.

compile_direct(Schema, []) ->
  glazer:compile_schema(Schema);
compile_direct(Schema, Options) ->
  glazer:compile_schema(Schema, Options).

%% The NIF expects the {schema, Resource} tuple returned by compile/1
-doc """
Validate `Data` against a compiled schema or a registered name.

`Schema` is a handle from `compile/1,2` (`{schema, Resource}` or the bare
resource), or the name of a registered schema. Any other term is taken as a name.
Returns `ok`, the error tuples of the validator, or `{error, {unknown_schema, Name}}`
for a name that is not registered.
""".
-spec validate(compiled() | name(), term()) -> ok | {error, term()}.
validate(Schema, Data) ->
  validate(Schema, Data, []).

-spec validate(compiled() | name(), term(), list()) -> ok | {error, term()}.
validate(Schema, Data, Options) ->
  case compiled_handle(Schema) of
    {ok, Compiled} ->
      validate_compiled(Compiled, Data, Options);
    error ->
      case fetch(Schema) of
        {ok, Compiled}     -> validate_compiled(Compiled, Data, Options);
        {error, not_found} -> {error, {unknown_schema, Schema}}
      end
  end.

compiled_handle({schema, Ref} = Compiled) when is_reference(Ref) -> {ok, Compiled};
compiled_handle(Ref) when is_reference(Ref)                      -> {ok, {schema, Ref}};
compiled_handle(_)                                               -> error.

validate_compiled(Compiled, Data, []) ->
  glazer:schema_validate(Compiled, Data);
validate_compiled(Compiled, Data, Options) ->
  glazer:schema_validate(Compiled, Data, Options).

-doc """
Compile `Schema` and validate `Data` in one call. Each call compiles the schema
again, so use `compile/1,2` and `validate/2,3` for repeated validation.
""".
-spec validate_term(map() | boolean(), term()) -> ok | {error, term()}.
validate_term(Schema, Data) ->
  glazer:schema_validate_term(Schema, Data).

-spec validate_term(map() | boolean(), term(), list()) -> ok | {error, term()}.
validate_term(Schema, Data, Options) ->
  glazer:schema_validate_term(Schema, Data, Options).


