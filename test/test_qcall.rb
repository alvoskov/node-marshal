# encoding: UTF-8
require_relative '../lib/node-marshal.rb'
require 'test/unit'


# This unit tests Ruby 2.3 &. safe navigation operator
class TestQCALL < Test::Unit::TestCase
	def test_qcall
		qcall_program = %q{
class Account
	def initialize(owner_name, owner_address)
		@owner_name = owner_name
		@owner_info = (owner_address.nil?) ? nil : OwnerInfo.new(owner_address);
	end
	def owner_name
		@owner_name
	end
	def owner_info
		@owner_info
	end
end
class OwnerInfo
	def initialize(address)
		@address = address
	end
	def address
		@address
	end
end
a = Account.new("Owner", "Moscow");
puts "'#{a&.owner_name}'"
puts "'#{a&.owner_info&.address}'"
b = Account.new("Owner", nil);
puts "'#{b&.owner_name}'"
puts "'#{b&.owner_info&.address}'"
[a&.owner_name, a&.owner_info&.address, b&.owner_name, b&.owner_info&.address]
}

		node = NodeMarshal.new(:srcmemory, qcall_program)
		bindump =  node.to_bin
		node = NodeMarshal.new(:binmemory, bindump)
		res_node = node.compile.eval
		res_text = eval(qcall_program)
		assert_equal(res_text, res_node)
	end
end
