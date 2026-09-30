app_path = "_build/dev/lib/glazer"  # main project
is_dependency_build = String.starts_with?(app_path, "/") and
                      not String.contains?(app_path, File.cwd!() <> "/_build")

IO.puts("app_path: #{app_path}")
IO.puts("cwd: #{File.cwd!()}")
IO.puts("is_dependency: #{is_dependency_build}")
IO.puts("")

app_path2 = "/home/s.aleynikov/projects/erl-libs/glazer/integration/_build/dev/lib/glazer"  # dependency
cwd2 = "/home/s.aleynikov/projects/erl-libs/glazer/integration/deps/glazer"
is_dependency_build2 = String.starts_with?(app_path2, "/") and
                       not String.contains?(app_path2, cwd2 <> "/_build")

IO.puts("app_path2: #{app_path2}")
IO.puts("cwd2: #{cwd2}")
IO.puts("is_dependency2: #{is_dependency_build2}")
