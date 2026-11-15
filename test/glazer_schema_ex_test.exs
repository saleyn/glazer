defmodule Glazer.SchemaTest do
  use ExUnit.Case, async: false

  @schema %{
    "type" => "object",
    "required" => ["name"],
    "properties" => %{"name" => %{"type" => "string", "minLength" => 1}}
  }

  test "compile and validate" do
    {:ok, compiled} = Glazer.Schema.compile(@schema, registry: false)
    assert :ok = Glazer.Schema.validate(compiled, %{"name" => "Ann"})

    assert {:error, {:path, ["name"], {:string_too_short, ""}}} =
             Glazer.Schema.validate(compiled, %{"name" => ""})
  end

  test "validate_term compiles and validates in one call" do
    assert :ok = Glazer.Schema.validate_term(@schema, %{"name" => "Ann"}, registry: false)
  end

  test "register, validate by name, unregister" do
    name = "elixir-test-user"
    {:ok, 1} = Glazer.Schema.register(name, @schema)
    assert {:ok, 1} = Glazer.Schema.version(name)
    assert :ok = Glazer.Schema.validate(name, %{"name" => "Ann"})
    assert {:error, {:unknown_schema, "no-such-schema"}} = Glazer.Schema.validate("no-such-schema", %{})
    assert :ok = Glazer.Schema.unregister(name)
    assert {:error, :not_found} = Glazer.Schema.fetch(name)
  end

  test "compile_file reads by extension" do
    dir = System.tmp_dir!()
    json = Path.join(dir, "glazer_schema_ex_#{System.unique_integer([:positive])}.JSON")
    yaml = Path.join(dir, "glazer_schema_ex_#{System.unique_integer([:positive])}.yml")
    File.write!(json, ~s({"type": "string"}))
    File.write!(yaml, "type: string\n")

    assert {:ok, c1} = Glazer.Schema.compile_file(json, registry: false)
    assert {:ok, c2} = Glazer.Schema.compile_file(yaml, registry: false)
    assert :ok = Glazer.Schema.validate(c1, "x")
    assert :ok = Glazer.Schema.validate(c2, "x")

    assert {:error, {:unsupported_format, ".txt"}} =
             Glazer.Schema.compile_file(Path.join(dir, "x.txt"))

    File.rm!(json)
    File.rm!(yaml)
  end
end
