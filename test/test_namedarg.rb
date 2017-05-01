# encoding: UTF-8
require_relative '../lib/node-marshal.rb'
require 'test/unit'


# This unit tests Ruby 2.0 named arguments support
class TestNamedArg < Test::Unit::TestCase
	def test_namedarg
		# For Ruby 2.1 and higher
		program_21 = %q{
def testfunc(x, foo:, bar: 'default', stuff:)
	x.to_s + " " + foo.to_s + " " + bar.to_s + " " + stuff.to_s
end
def blockfunc(&block)
	block.call(foo: 'bar')
end

blockstr = blockfunc do |foo:|
	foo
end

testfunc(1, foo: 'a', stuff: 1e5) + "\n" +
	testfunc(1, stuff: -50, foo: 'a', bar: 1.23e10) + "\n" + blockstr + "\n"
}
		# For Ruby 2.0
		program_20 = %q{
def testfunc(x, bar: 'default')
	x.to_s + " " + bar.to_s
end
testfunc(1) + "\n" + testfunc(1, bar: 'default')
}
		# Detect version of Ruby
		ver = RUBY_VERSION
		ver = (ver[0] + ver[2] + ver[4]).to_i
		# Version-specific stuff
		program = ""
		if ver >= 210
			program = program_21
		elsif ver >= 210
			program = program_20
		else
			assert_equal(false, true, "Ruby 2.0 or higher is required for named arguments test")
		end
		# Test source code compilation
		node = NodeMarshal.new(:srcmemory, program)
		node.show_offsets = true
		#puts node.dump_tree_short
		bindump = node.to_bin
		node = NodeMarshal.new(:binmemory, bindump)
		res_node = node.compile.eval
		res_text = eval(program)
		assert_equal(res_text, res_node)
	end
end
