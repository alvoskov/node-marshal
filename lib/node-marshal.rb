require_relative '../ext/node-marshal/nodemarshal.so'
require 'zlib'

class NodeMarshal
	# call-seq:
	#   obj.to_compiled_rb(outfile, opts)
	#
	# Transforms node to the Ruby file
	# - +outfile+ -- name of the output file
	# - +opts+ -- Hash with options (+:compress+, +:so_path+)
	#   +:compress+ can be +true+ or +false+, +:so_path+ is a test string 
	#   with the command for nodemarshal.so inclusion (default is 
	#   require_relative '../ext/node-marshal/nodemarshal.so')
	def to_compiled_rb(outfile, *args)
		compress = false
		so_path = "require_relative '../ext/node-marshal/nodemarshal.so'"
		if args.length > 0
			opts = args[0]
			if opts.has_key?(:compress)
				compress = opts[:compress]
			end
			if opts.has_key?(:so_path)
				so_path = opts[:so_path]
			end
		end
		# Compression
		if compress
			zlib_include = "require 'zlib'"
			data_txt = NodeMarshal.base85r_encode(Zlib::deflate(self.to_bin))
			data_bin = "Zlib::inflate(NodeMarshal.base85r_decode(data_txt))"
		else
			zlib_include = "# No compression"
			data_txt = self.to_text
			data_bin = "NodeMarshal.base85r_decode(data_txt)"
		end
		# Document header
		txt = <<EOS
# Ruby compressed source code
# RUBY_PLATFORM: #{RUBY_PLATFORM}
# RUBY_VERSION: #{RUBY_VERSION}
#{zlib_include}
#{so_path}
data_txt = <<DATABLOCK
#{data_txt}
DATABLOCK
data_bin = #{data_bin}
node = NodeMarshal.new(:binmemory, data_bin)
node.filename = __FILE__
node.filepath = File.expand_path(node.filename)
node.compile.eval
EOS
		# Process input arguments
		if outfile != nil
			File.open(outfile, 'w') {|fp| fp << txt}
		end
		return txt
	end

	def self.compile_rb_file(outfile, inpfile, *args)
		node = NodeMarshal.new(:srcfile, inpfile)
		node.to_compiled_rb(outfile, *args)
		return true
	end
end
