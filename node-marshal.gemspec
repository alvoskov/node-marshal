Gem::Specification.new do |s|
	s.bindir = ['bin']
	s.executables = ['noderbc']
	s.files = 
		Dir.glob("README.rdoc") + 
		Dir.glob("COPYING") +
		Dir.glob("lib/*.rb") + 
		Dir.glob("lib/node-marshal/*.rb") + 
		Dir.glob("test/test_*.rb") +
		Dir.glob("test/lifegame.rb") +
		Dir.glob("test/tinytet.rb") +
		Dir.glob("ext/node-marshal/extconf.rb") +
		Dir.glob("ext/node-marshal/libobj/readme.txt") + 
		Dir.glob("ext/node-marshal/*.c") +
		Dir.glob("ext/node-marshal/*.h") +
		Dir.glob("bin/noderbc.bat") +
		Dir.glob("bin/noderbc")
	s.homepage = "https://github.com/dig386/node-marshal"
	s.extensions = "ext/node-marshal/extconf.rb"
	s.name = "node-marshal"
	s.summary = "Transforms Ruby sources to binary nodes (trees) that can be saved and loaded"
	s.description = <<EOS
This gem is designed for transformation of Ruby source code (eiher in the form of files or strings) to the 
Ruby nodes (syntax trees) used by Ruby MRI internals. Obtained nodes can be serialized to the platform-dependent
binary or ASCII strings and restored and launched from serialized format. Such kind of transformation is
irreversible and can be used for source code protection; the similar principle is used by RubyEncoder commercial
software.
EOS
	s.author = "Alexey Voskov"
	s.version = "0.2.2"
	s.license = "BSD-2-Clause"
	s.email = "alvoskov@gmail.com"
	s.required_ruby_version = ">= 1.9.3"

	s.extra_rdoc_files = ['README.rdoc']
	s.rdoc_options << '--main' << 'README.rdoc'
end
