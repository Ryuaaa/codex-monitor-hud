require 'json'
root = File.expand_path('..', __dir__)
catalog = JSON.parse(File.read(File.join(root, 'overlay/HUDLocalizations.json')))
formats = ->(s) { s.scan(/%%|%(?:\d+\$)?[-+ #0]*(?:\d+|\*)?(?:\.\d+)?(?:ll|l|z)?[@difugs]/) }
failures = []
catalog.each do |key, translations|
  %w[en ja ko].each do |language|
    value = translations[language]
    failures << "missing #{language}: #{key}" unless value.is_a?(String) && !value.empty?
    failures << "format mismatch #{language}: #{key}" unless value && formats.call(value) == formats.call(key)
  end
end
Dir[File.join(root, 'overlay/*.m')].each do |path|
  File.read(path).scan(/HUDL\(@"((?:\\.|[^"\\])*)"\)/).flatten.each do |key|
    failures << "uncatalogued: #{key}" unless catalog.key?(key)
  end
end
abort failures.join("\n") unless failures.empty?
puts "localizations=#{catalog.size} languages=5 placeholder_and_coverage_checks=pass"
