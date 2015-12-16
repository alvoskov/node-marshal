# encoding: UTF-8

require_relative '../lib/node-marshal.rb'
require 'test/unit'

# This unit test case contains several very simple "smoke tests"
# for NodeMarshal class
class TestNodeMarshal < Test::Unit::TestCase
	# Test: Hello, World
	def test_hello
		program = <<-EOS
puts "Hello, World"
"Hello, World"
EOS
		assert_node_compiler(program)
	end

	# Test: sum of squares
	def test_sum_of_squares
		program = <<-EOS
sum = 0
(1..50).each do |n|
	sum += n ** 2
end
sum
EOS
		assert_node_compiler(program)
	end

	# Test: factorial calculation
	def test_factorials
		program = <<-EOS
ni = [1, 2, 3, 4, 5, 6, 7, 8, 9]
def fact(n)
	(n == 1) ? 1 : n * fact(n - 1)
end
ni.map {|x| fact(x) }
EOS
		assert_node_compiler(program)		
#		test_string_eval(program, [1, 2, 6, 24, 120, 720, 5040, 40320, 362880], "Factorial");
	end

	# Simple ROT13 task that combines several language construction
	def test_rot13
		program = <<-EOS
class ROT13 < String
	def initialize(str)
		@rfunc = Proc.new do |chr, limit|
			newcode = chr.ord + 13
			newcode -= 26 if newcode > limit.ord
			newcode.chr
		end
		super(str)
	end

	def rot13
		ans = ""
		self.each_char do |c|
			case c
			when ('A'..'Z')
				ans += @rfunc.(c,'Z')
			when ('a'..'z')
				ans += @rfunc.(c,'z')
			else
				ans += c
			end    
		end
		return ROT13.new(ans)
	end
end

$str = ROT13.new("This is global variable string!")
[$str.rot13, $str.rot13.rot13]
EOS
		assert_node_compiler(program)
	end

	# Test: array initialization and operations
	def test_array
		program = <<-EOS
[2*2, 3*3, 4*5 + 6, [5,6,7], true && false].flatten
EOS
		assert_node_compiler(program)
	end

	# Test: Eratosthenes algorithm for searching prime numbers
	def test_eratosthenes_primes
		program = <<-EOS
table = Array.new(5000) {|index| index + 2}
table.each_index do |ind|
	num = table[ind]
	if num != 0 
		((ind + num)..(table.length)).step(num) {|i| table[i] = 0 }
	end
end
table.select! {|val| val != 0}
table
EOS
		assert_node_compiler(program)
	end

	# Test: classes declaration and inheritance
	def test_classes
		program = <<-EOS
class XYPoint
	attr_reader :x, :y
	def initialize(x, y)
		@x = x.to_f
		@y = y.to_f
	end

	def distance(p)
		return ((@x - p.x)**2 + (@y - p.y)**2)**0.5
	end	
end

class PolarPoint < XYPoint
	def initialize(r, phi)
		phi = phi * (2*Math::PI) / 360
		super(r*Math::sin(phi), r*Math::cos(phi))
	end
end

a, b = XYPoint.new(0.0, 0.0), XYPoint.new(3.0, 4.0)
c, d = PolarPoint.new(1.0, 0.0), PolarPoint.new(2.0, 90.0)
[a.distance(b), c.distance(d)]
EOS
		assert_node_compiler(program)
	end

	# Test: Base85r encoders and decoders
	def test_base85r
		base85_pass = ->str{NodeMarshal.base85r_decode(NodeMarshal.base85r_encode(str))}
		# Short strings
		assert_equal("", base85_pass.(""))
		assert_equal(" ", base85_pass.(" "))
		assert_equal("AB", base85_pass.("AB"))
		assert_equal("ABC", base85_pass.("ABC"))
		assert_equal("ABCD", base85_pass.("ABCD"))
		assert_equal("ABCDE", base85_pass.("ABCDE"))
		# Random strings
		rnd = Random.new
		20.times do
			len, str = rnd.rand(4096), ""
			len.times { str += rnd.rand(255).chr }
			assert_equal(str, base85_pass.(str))
		end
	end

	# Test regular expressions (NODE_MATCH3 node issue)
	def test_node_match3
		program = <<-EOS
a = "  d--abc"
a =~ Regexp.new("abc")
EOS
		assert_node_compiler(program)
	end

	def test_node_block_pass
		program = <<-EOS
[1, 2, 3, "A"].map(&:class)
EOS
		assert_node_compiler(program)
	end

	# Part of the tests: compare result of direct usage of eval
	# for the source code and its
	def assert_node_compiler(program)
		res_eval = eval(program)
		node = NodeMarshal.new(:srcmemory, program)
		bin = node.to_bin; node = nil; GC.start
		node = NodeMarshal.new(:binmemory, bin)
		res_node = node.compile.eval
		assert_equal(res_node, res_eval, "Invalid result from the compiled node")
	end
end
