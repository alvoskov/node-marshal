require_relative '../lib/node-marshal.rb'
require 'test/unit'

# Wrapper for Ruby Proc class (Proc and lambdas) that
# saves the text representation of the function
class TestFunc
	def initialize(text)
		@text = text.to_s
		@value = eval(text)
	end

	def to_s
		@text
	end

	def inspect
		@text
	end

	def call(x,a)
		@value.(x,a)
	end
end

# Class for generation of the complex structure that includes:
# Hashes, Arrays, Floats, Fixnums, Ranges, Strings, Symbols, Procs
# It is used for node testing
class TestHashTree
	attr_reader :value, :depth
	def initialize(*args)
		if args.length == 2
			rnd = Random.new
			lambdas = ['->x,a{a*x**2 + 2*x + 1}', '->x,a{Math::sin(a) + x**2 * a**0.5}']
			data_size, depth = args[0], args[1]
			@depth = depth
			@value = {:depth => depth, :data => [], :leaves =>[] }
			data_size.times do
				case rnd.rand(30000) % 7
				when 0
					@value[:data] << 10**(rnd.rand(600000) * 0.001 - 300)
				when 1
					@value[:data] << rnd.rand(2**31 - 1)
				when 2
					rndstr = (Array.new(25) { (rnd.rand(127 - 32) + 32).chr }).join
					@value[:data] << rndstr
				when 3
					@value[:data] << TestFunc.new(lambdas[rnd.rand(30000) % lambdas.length])
				when 4
					a, b = rnd.rand(100), rnd.rand(10)
					@value[:data] << (a..(a + b))
				when 5
					a, b = rnd.rand(100), rnd.rand(10)
					@value[:data] << (a...(a + b))
				when 6
					rndsym = (Random.rand(1_000_000_000).to_s(36)).to_sym
					@value[:data] << rndsym
				else
					nil
				end
			end
	
			if depth > 0
				3.times do 
					@value[:leaves] << TestHashTree.new(data_size, depth - 1)
				end
			end
		elsif args.length == 1
			h = args[0]			
			raise(ArgumentError, 'Input argument must be a hash') if !h.is_a?(Hash)
			@depth = h[:depth]
			@value = {
				:depth => @depth,
				:data => h[:data].map {|x| (x.class == String) ? x.dup : x },
				:leaves => h[:leaves].map {|x| TestHashTree.new(x)}}
		else
			raise ArgumentError, 'Invalid numer of arguments'
		end
	end

	def to_s
		@value.to_s
	end

	def inspect
		self.to_s
	end
	# Recursive comparison of two complex structures
	def ==(val)
		if (self.depth != val.depth) ||
		   (self.value[:data].length != val.value[:data].length)
			return false
		else
			# Compare data
			res = true
			v1, v2 = self.value[:data], val.value[:data]
			v1.each_index do |ind|
				if (v1[ind].class != Proc && v1[ind].class != TestFunc)
					res = false if v1[ind] != v2[ind]
				else
					x, a = 1.2345, 1.5
	                                y1, y2 = v1[ind].call(x,a), v2[ind].call(x,a)
					res = false if y1 != y2
				end
			end
			# Compare leaves
			p1, p2 = self.value[:leaves], val.value[:leaves]
			p1.each_index {|ind| res = false if p1[ind] != p2[ind] }
			return res
		end
	end
end

# Tests that check the processing of large and/or complex nodes
class TestComplex < Test::Unit::TestCase
	def test_bruteforce
		# Create test tree, turn it to node and save it to disk
		puts 'Creating the tree...'
		tree_src = TestHashTree.new(20, 7);
		puts 'Dumping the node...'
		tree_str = tree_src.to_s
		node = NodeMarshal.new(:srcmemory, tree_str)
		tree_bin = node.to_bin

		File.open('node.bin', 'wb') {|fp| fp << tree_bin }
		puts "  Source code size: %d bytes" % tree_str.length
		puts "  Binary data size: %d bytes" % tree_bin.length
		# Clear the memory
		node = nil; GC.enable; GC.start
		# Load the node from the disk and turn it to tree
		puts 'Loading the node...'
		node = NodeMarshal.new(:binfile, 'node.bin')
		puts node.inspect
		tree_dest = TestHashTree.new(node.compile.eval)
		assert_equal(tree_src, tree_dest)
	end
end