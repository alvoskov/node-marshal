# Implementation of simple Convay's life game designed mainly
# for node compiler testing
#
# (C) 2015 Alexey Voskov
# License: 2-clause BSD
module LifeGame

# Representation of the cell of the game grid (field)
# Keeps the state of the cell (EMPTY, FILLED, BORN, DEAD)
# and neighbours of the cell (as array of pointers to another Cell
# object examples)
class Cell
	EMPTY = 0
	FILLED = 1
	BORN = 2
	DEAD = 3

	attr_reader :value, :neighbours
	def initialize
		@value = EMPTY
		@neighbours = []
	end

	def value=(val)
		@value = val.to_i
	end
	
	def neighbours=(val)
		@neighbours = val
	end

	def chr
		case @value
		when EMPTY; '.'
		when FILLED; 'O'
		when BORN; '+'
		when DEAD; '-'
		else; '?'
		end
	end

	def update
		case @value
		when BORN; @value = FILLED
		when DEAD; @value = EMPTY
		end
	end
end

# Convay's life game grid (field)
class Grid
	public
	def initialize(height, width)
		width, height = width.to_i, height.to_i
		raise "Invalid value of width" if (width < 3 || width > 100)
		raise "Invalid value of height" if (height < 3 || height > 100)
		@width, @height = width, height

		@f = Array.new(@height) {|ind| Array.new(@width) {|ind| Cell.new }}
		# Set neighbours for each cell
		@@xy_shifts = [[-1, -1], [-1, 0], [-1, 1],
			[0, -1], [0, 1],
			[1, -1], [1, 0], [1, 1]]
		(0...@height).each do |y|
			(0...@width).each do |x|
				# Calculate neighbours coordinates
				xy = @@xy_shifts.map do |elem|
					q = [elem[0] + y, elem[1] + x]
					(q[0] < 0 || q[0] >= @height || q[1] < 0 || q[1] >= @width) ? nil : q
				end
				xy.compact!
				# And transform them to the matrix
				@f[y][x].neighbours = xy.map {|q| @f[q[0]][q[1]] }
			end
		end
		self
	end
	# Initialize game field with one glider
	def cfg_glider!
		self.clear!
		@f[1][2].value = Cell::FILLED
		@f[2][3].value = Cell::FILLED
		@f[3][1].value = Cell::FILLED
		@f[3][2].value = Cell::FILLED
		@f[3][3].value = Cell::FILLED
	end
	# Initialize game field with glider gun
	def cfg_glider_gun!
		self.clear!
		gun = [
		'........................O...........',
		'......................O.O...........',
		'............OO......OO............OO',
		'...........O...O....OO............OO',
		'OO........O.....O...OO..............',
		'OO........O...O.OO....O.O...........',
		'..........O.....O.......O...........',
		'...........O...O....................',
		'............OO......................'];
		yshift, xshift = 2, 2
		gun.each_index do |yi|
			line = gun[yi]
			(0..line.length).each {|xi| @f[yi+yshift][xi+xshift].value = Cell::FILLED if gun[yi][xi] == 'O'}
		end
	end
	# Clear game field
	def clear!
		@f.each do |line|
			line.each { |cell| cell.value = Cell::EMPTY }
		end
	end
	# Convert game field to ASCII string (best viewed when typed in
	# monospaced font). Suitable for autotesting
	def to_ascii
		txt = ""
		@f.each do |line|
			line.each { |field| txt += field.chr }
			txt += "\n"
		end
		return txt
	end
	# Make one step (turn)
	def make_step!
		# Cells birth
		@f.each_index do |yi|
			@f[yi].each_index do |xi|
				n = cell_neighbours_num(yi, xi)
				@f[yi][xi].value = Cell::BORN if (@f[yi][xi].value == Cell::EMPTY && n == 3)
				@f[yi][xi].value = Cell::DEAD if (@f[yi][xi].value == Cell::FILLED && !(n == 2 || n == 3))
			end
		end
		# Cells update
		@f.each do |line|
			line.each {|val| val.update}
		end
		self
	end

	private
	def cell_neighbours_num(y, x)
		(@f[y][x].neighbours.select {|q| q.value == Cell::FILLED || q.value == Cell::DEAD }).length
	end
end

end
