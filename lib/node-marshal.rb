require_relative '../ext/node-marshal/nodemarshal.so'
begin
	require 'zlib'
rescue LoadError
	# If zlib library is absent in the system no support for
	# compression will be provided in 
end

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
			if !defined?(Zlib)
				raise "Compression is not supported: Zlib is absent"
			end
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

	# call-seq:
	#   obj.replace_symbols(syms_subs)
	#
	# Replaces some symbols inside parsed AST to user-defined aliases.
	# It is designed to make code obfuscation easier. Be careful when using
	# this ability: it is possible to break external libraries calls,
	# operators overloading and some metaprogramming techniques.
	# - +syms_subs+ -- Hash with the table of aliases. Keys are original names,
	# values are aliases. Keys and values MUST BE strings (not symbols!).
	def replace_symbols(syms_subs)
		# Check input data
		# a) type
		if !(syms_subs.is_a?(Hash))
			raise "symb_subs must be a hash"
		end
		# b) uniqueness of values inside the hash
		values = syms_subs.values
		if values.size != values.uniq.size
			raise ArgumentError, "values (new names) must be unique"
		end
		# c) uniqueness of values after replacement
		# TODO: MAKE IT!!!
		# Use NodeMarshal C part to replace the symbols
		self.to_hash # To initialize Hash with preparsed Ruby AST NODE
		syms_subs.each do |key, value|
			change_symbol(key, value)
		end
		self
	end

	# call-seq:
	#   obj.get_safe_symbols
	#
	# Returns an array that contains strings with the names of symbols that are safe
	# to change. It excludes symbols that are present in the table of literals (and their derivatives
	# such as @x and x=). Such operation is useful for attr_readed, attr_writer and another similar
	# metaprogramming techniques handling
	#
	# - +our_symbols+ symbols created during node creation (must be found manually by the user
	#   by means of Symbol.all_symbols calling BEFORE and AFTER node creation.
	def get_safe_symbols(our_symbols)
		self.to_hash # To initialize Hash with preparsed Ruby AST NODE
		symbolic_literals =  self.literals.select {|x| x.is_a?(Symbol)}.map {|x| x.to_s}
		fixed_symbols = [] + symbolic_literals
		fixed_symbols += symbolic_literals.map {|x| "@#{x}"}
		fixed_symbols += symbolic_literals.map {|x| "#{x}="}
		our_symbols = our_symbols.dup
		our_symbols -= fixed_symbols
	end

	# call-seq:
	#   obj.get_aliases_table(our_symbols)
	# 
	# Returns a hash that has {"old_sym_name"=>"new_sym_name",...} format.
	# "new_sym_name" are generated automatically.
	#
	# - +our_symbols+ -- An array that contains the list of symbols (AS STRINGS,
	# NOT AS SYMBOLS) that can be renamed.
	def get_aliases_table(our_symbols)
		symbols_ary = get_safe_symbols(our_symbols)
		pos = 0;
		aliases_ary = symbols_ary.map do |sym|
			pos += 1
			if sym.length > 1 && sym[0..1] == '@@'
				"@@q#{pos}"
			elsif sym[0] == '@'
				"@q#{pos}"
			elsif sym[0] =~ /[A-Z]/
				"Q#{pos}"
			elsif sym[0] =~ /[a-z]/
				"q#{pos}"
			end
		end
		[symbols_ary, aliases_ary].transpose.to_h		
	end

	# call-seq:
	#   obj.rename_ivars
	def rename_ivars(*args)
		if args.size == 0
			excl_names = []
		else
			excl_names = args[0]
		end

		to_hash
		syms = @nodehash[:symbols].select {|x| (x =~ /@[^@]/) == 0}
		pos = 1;
		syms_new = syms.map do |x|
			if excl_names.find_index(x[1..-1]) != nil
				str = x
			else
				str = "@ivar#{pos}"
			end
			pos = pos + 1;
			str
		end
		syms_subs =  [syms, syms_new].transpose.to_h
		replace_symbols(syms_subs)
		self
	end

	# call-seq:
	#   obj.rebuld
	#
	# Rebuilds the node by converting it to the binary dump and further restoring
	# of it from this dump. It doesn't change the original node and returns rebuilt
	# node.
	def rebuild
		NodeMarshal.new(:binmemory, to_bin)
	end
end

# Designed for the logging of symbols table changes. When an example of 
# SymbolsLogger is created the current global table of symbols is saved 
# inside it. The created example can be used for finding new symbols in
# the global table. This is useful for code obfuscation.
class SymbolsLogger
	def initialize
		@symtbl_old = Symbol.all_symbols
	end

	def new_symbols
		symtbl_new = Symbol.all_symbols
		symtbl_new - @symtbl_old
	end

	def update
		@symtbl_old = Symbol.all_symbols
	end
end
