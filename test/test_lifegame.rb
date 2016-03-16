require_relative '../lib/node-marshal.rb'
require 'test/unit'

# Set of tests for Convay's life game running and compilation
# (includes tests with binary Ruby files)
class TestLifeGame < Test::Unit::TestCase
	# Compile and launch life game with different
	# compiler options (requires zlib library)

	def test_compile
		compile_with_opts(nil)
		compile_with_opts(:compress=>true)
	end
	# Test life game with glider gun configuration
	def test_glider_gun
		# Compile the class to the instuction sequence
		bin = NodeMarshal.new(:srcfile, 'lifegame.rb')
		puts bin.inspect
		bin.compile.eval
		# Calculate the life game
		res_node = run_game
		# Play the game of life without NodeMarshal
		Object.send(:remove_const, :LifeGame)
		load('lifegame.rb')
		res_load = run_game
		# Compare the results
		assert_equal(res_node, res_load)
		Object.send(:remove_const, :LifeGame)
		puts res_load
	end

	# Runs Life game initialized with "glider gun" configuration	
	# and makes 75 turns (changes of generations)
	def run_game
		g = LifeGame::Grid.new(25, 80)
		g.cfg_glider!
		g.cfg_glider_gun!
		75.times {g.make_step!}
		g.to_ascii
	end

	# Compiles Convay's life game using proposed options
	# (opts is a Hash, see NodeMarshal#compile_rb_file method)
	def compile_with_opts(opts)
		stub = <<-EOS
g = LifeGame::Grid.new(25, 80)
g.cfg_glider_gun!
75.times {g.make_step!}
File.open('life.res', 'w') {|fp| fp << g.to_ascii }
EOS
		# Compile Life game to the file and obtain the result
		if opts == nil
			NodeMarshal.compile_rb_file('lifegame_bin.rb', 'lifegame.rb')
		else
			NodeMarshal.compile_rb_file('lifegame_bin.rb', 'lifegame.rb', opts)
		end
		txt = File.read('lifegame_bin.rb') + stub
		File.open('lifegame_bin.rb', 'w') {|fp| fp << txt }
		`ruby lifegame_bin.rb`
		res_file = File.read('life.res')
		`rm life.res`
		# Run the life game without compilation to the file
		load('lifegame.rb')
		res_load = run_game
		Object.send(:remove_const, :LifeGame)
		assert_equal(res_file, res_load)
	end
end
