app_path = "_build/dev/lib/glazer"
is_dependency_build = not File.exists?("src/glazer.app.src")
IO.puts("app_path: #{app_path}")
IO.puts("src exists: #{File.exists?("src/glazer.app.src")}")
IO.puts("is_dependency_build: #{is_dependency_build}")
IO.puts("target would be: #{if is_dependency_build, do: "compile", else: "optimize"}")
