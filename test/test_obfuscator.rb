require_relative '../lib/node-marshal.rb'
require 'test/unit'

# Test node-marshal obfuscator abilities
class TestObfuscator < Test::Unit::TestCase
	def test_sym_replace
		# Create the node and find all new symbols
		symlog = SymbolsLogger.new
		node = NodeMarshal.new(:srcfile, 'lifegame.rb')
		our_symbols = symlog.new_symbols.map(&:to_s)
		node.to_hash
		# Try to exclude symbols used in attr_reader and attr_writer constructions
		our_symbols = node.get_safe_symbols(our_symbols)
		# Prepare hash table for replacement
		reptbl = node.get_aliases_table(our_symbols)
		life_game_name = reptbl["LifeGame"]
		grid_name = reptbl["Grid"]
		make_step_name = reptbl["make_step!"].to_sym
		cfg_glider_gun_name = reptbl["cfg_glider_gun!"].to_sym
		# Replace symbols
		puts "----- Symbols replacement table"
		puts reptbl.to_s
		puts "-------------------------------"
		node.replace_symbols(reptbl)
		# Rebuild node, save it to the file and reload it
		node = node.rebuild
		File.open('life.bin', 'wb') {|fp| fp << node.to_bin};
		node.compile.eval
		# Execute the node
		grid_class = Object.const_get(life_game_name).const_get(grid_name)
		g = grid_class.new(25, 80)
		g.send(cfg_glider_gun_name)
		75.times {g.send(make_step_name)}
		puts g.to_ascii
	end
end
