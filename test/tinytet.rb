#!/usr/bin/ruby
# Simple TETRIS game for Ruby 1.9.x that uses CURSES library
# (C) 2013 Alexey Voskov
require "curses"
include Curses
# TETRIS figure
class Figure
	attr_accessor :mat, :x, :y, :rot
	FIGURES = [0x0F00,0x0660,0x0270,0x0170,0x0470,0x0360,0x0C60] # Line, square, T, L, L, Z, Z
	def initialize
		fig = FIGURES[rand(6).round]
		rf = ->bf{Array.new(4) {|y| Array.new(4) {|x| (1 << bf.(x,y)) & fig }}}
		@mat = [rf.(->x,y{3-x+y*4}), rf.(->x,y{3-y+(3-x)*4}), 
			rf.(->x,y{x+(3-y)*4}), rf.(->x,y{y+x*4})]
		@x, @y, @rot = 5, 0, 0 # Figure position
	end
	def each_pos &block
		(0..3).each {|y| (0..3).each {|x| block.(y,x) if @mat[@rot][y][x] > 0}}
	end
	# Test the possibility of move and move if possible
	def move!(op, unop, scr)
		self.each_pos {|y,x| scr.brick!(y+@y,x+@x,GameField::COL_EMPTY) }
		op.(self)
		unop.(self) if !(ans = scr.place?self)
		self.each_pos {|y,x| scr.brick!(y+@y,x+@x,GameField::COL_BRICK) }
		refresh
		ans
	end
end
# TETRIS game fields
class GameField
	WIDTH, HEIGHT, BRICK_WIDTH, COL_EMPTY, COL_WALL, COL_BRICK = 16, 26, 2, 0, 1, 2
	def initialize # Initialize and show the game fields
		@m = Array.new(HEIGHT+2) {[0,0,1,Array.new(WIDTH-6){0},1,0,0].flatten}
		(2..WIDTH-3).each {|i| @m[HEIGHT-1][i] = COL_WALL}
		lines!
	end
	def brick!(y,x,c) # Show the brick on the screen
		@m[y][x] = c.to_i
		return nil if y-3 <= 0
		setpos(y-3,x*BRICK_WIDTH)
		addstr([". ", "**", "[]"][c])
	end
	def place?(fig) # Check if the figure can be placed
		fig.each_pos {|y,x| return false if @m[y+fig.y][x+fig.x] > 0}
		return true
	end
	def lines! # Erase full lines from the screen
		while (ind = @m[0..HEIGHT-2].index {|s| s[3..WIDTH-4].index(COL_EMPTY) == nil}) != nil
			(ind-1).step(0,-1) {|i| @m[i+1] = @m[i]}
			@m[0] = @m[HEIGHT+1].dup
		end
		(0..HEIGHT-1).each {|y| (2..WIDTH-3).each {|x| brick!(y,x,@m[y][x])}} # Update the screen (field + borders) 	
	end
end
# Initialize the console and program data
init_screen; clear; noecho; stdscr.keypad(true)
fig, scr, speed = Figure.new, GameField.new, 1
# Keyboard control thread
keythread = Thread.new { loop {
	case getch
		when Key::LEFT then fig.move!(->f{f.x -=1}, ->f{f.x +=1}, scr)
		when Key::RIGHT then fig.move!(->f{f.x +=1}, ->f{f.x -=1}, scr)
		when Key::UP then fig.move!(->f{f.rot = (f.rot+1)%4}, ->f{f.rot = (f.rot+3)%4},scr)
		when Key::DOWN  then speed = 0.05 # Delay for fast falling
	end
}}
# Game main loop (new figure creation + falling)
begin
	fig, speed = Figure.new, 0.5 # Figure and delay for its normal falling
	sleep(speed) while fig.move!(->f{f.y +=1}, ->f{f.y -=1}, scr)
	scr.lines! 
end until fig.y == 0
# Finish the game
keythread.kill
setpos(GameField::HEIGHT/2,GameField::WIDTH-4)
addstr("GAME OVER!")
getch
close_screen
