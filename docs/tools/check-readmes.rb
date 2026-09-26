# Documentation-only link and language parity checks; no network or user data.
require 'pathname'
root = Pathname.new(__dir__).join('../..').realpath
files = %w[README.md README.zh-Hant.md README.en.md README.ja.md README.ko.md]
errors = []
files.each do |name|
  text = root.join(name).read
  errors << "#{name}: unbalanced details" unless text.scan(/<details>/).size == text.scan(%r{</details>}).size
  (text.scan(/\]\(([^\s)]+)\)/).flatten + text.scan(/src="([^"]+)"/).flatten).each do |target|
    next if target.start_with?('https://', 'http://', '#')
    path = target.split('#').first
    errors << "#{name}: missing #{path}" unless root.join(path).exist?
  end
  (files - [name]).each { |other| errors << "#{name}: missing language #{other}" unless text.include?("(#{other})") }
  %w[Codex-Monitor-HUD.app.zip CodexMonitorHUD-windows-x64-1.4.0.msi CodexMonitorHUD-windows-x64.zip].each do |asset|
    errors << "#{name}: missing direct download #{asset}" unless text.include?("/#{asset})")
  end
  %w[CNY USD EUR JPY KRW].each { |currency| errors << "#{name}: missing currency #{currency}" unless text.include?(currency) }
  intro = text.split('<a id="technical-details">').first
  errors << "#{name}: expected six feature bullets" unless intro.scan(/^- \*\*/).length == 6
  errors << "#{name}: expected three getting-started steps" unless intro.scan(/^\d\. /).length == 3
end
%w[zh-Hans zh-Hant en ja ko].each do |language|
  %w[home codex computer].each do |page|
    data = root.join("docs/images/#{page}-#{language}.png").binread
    errors << "invalid PNG #{page}-#{language}" unless data.start_with?("\x89PNG\r\n\x1a\n".b)
  end
  data = root.join("docs/images/tour-#{language}.gif").binread
  errors << "invalid GIF #{language}" unless data.start_with?('GIF89a', 'GIF87a')
end
abort errors.join("\n") unless errors.empty?
puts 'PASS: 5 language guides, matching downloads/features/onboarding, local links, 15 PNGs and 5 GIFs'
