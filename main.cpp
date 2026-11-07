#include <iostream>
#include <cstdint>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <type_traits>
#include <random>
#include <limits>
#include <sys/epoll.h>

#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>

volatile sig_atomic_t terminal_resized = 0;

void signalHandle(int signum) {
	if (signum == SIGWINCH) {
		terminal_resized = 1;
	}
}

static uint16_t width;
static uint16_t height;

// Helper Functions {{{
template<size_t N>
constexpr std::array<char, N> makeBorder() {
	std::array<char, N> result{};
	result[0] = '+'; result[N - 2] = '+'; result[N - 1] = '\0';
	for (size_t i = 1; i < N - 2; i++) result[i] = '-';
	return result;
}

template<size_t N>
constexpr std::array<char, N> makeSideBorder() {
	std::array<char, N> result{};
	result[0] = '|'; result[N - 2] = '|'; result[N - 1] = '\0';
	for (size_t i = 1; i < N - 2; i++) result[i] = ' ';
	return result;
}

template<size_t W, size_t H>
constexpr std::array<char, W*H+1> makeBoard() {
	std::array<char, W*H + 1> result{};
	result[W*H] = '\0';

	size_t b_wsz = W / 3 - 1;
	size_t b_hsz = H / 3 - 1;

	for (size_t h = 0; h < H; h++) {
		if (h % (b_hsz + 1) == 0) {
			for (size_t w = 0; w < W; w++) result[W * h + w] = '-';
			result[W * h] = '+';
			result[W * h + b_wsz + 1] = '+';
			result[W * h + (b_wsz + 1) *2] = '+';
			result[W * h + (b_wsz + 1) *3] = '+';
		} else {
			for (size_t w = 0; w < W; w++) result[W * h + w] = ' ';
			result[W * h] = '|';
			result[W * h + b_wsz + 1] = '|';
			result[W * h + (b_wsz + 1)*2] = '|';
			result[W * h + (b_wsz + 1)*3] = '|';
		}
	}

	return result;
}

template <typename T, typename N>
N getMiddle(T width, N sz) {
	static_assert(std::is_integral<T>::value && std::is_integral<N>::value, "getMiddle: T must be integral!");
	return (sz > width) ? 1 : 1 + ((N)width - sz) / 2;
}

template <typename T, typename N>
N getRight(T width, N sz) {
	static_assert(std::is_integral<T>::value && std::is_integral<N>::value, "getMiddle: T must be integral!");
	return (sz > width) ? 1 : ((N)width - sz);
}

template <typename Coord, typename Size>
bool isPointInside(Coord x, Coord y, Size w, Size h, Coord px, Coord py) {
	static_assert(std::is_integral<Coord>::value && std::is_integral<Size>::value, "isPointInside: Coord and Size must be integral!");
	return ((px >= x && px <= x + (Coord)w) && (py >= y && py <= y + (Coord)h));
}
// }}}

// Model {{{
enum class Opponent : uint8_t {
	Player,
	NormalBot,
	HardBot
};

struct Model {
	constexpr static char title[] = "Tic Tac Toe";
	constexpr static char start[] = "[S]tart";
	constexpr static char about[] = "[A]bout";
	constexpr static char exit[] = "[E]xit";

	constexpr static size_t start_but = sizeof(start) - 1 + 4;
	constexpr static size_t about_but = sizeof(about) - 1 + 4;
	constexpr static size_t exit_but = sizeof(exit) - 1 + 4;

	constexpr static std::array<char, start_but+1> start_border = makeBorder<start_but+1>();
	constexpr static std::array<char, about_but+1> about_border = makeBorder<about_but+1>();
	constexpr static std::array<char, exit_but+1> exit_border = makeBorder<exit_but+1>();

	constexpr static char about_title[] = "About";
	constexpr static char about_text[] =
		"Tic Tac Toe V1.0\n"
		"Created by Rechaift";

	constexpr static const char* role_msg[] = {
		"Play as X", "Play as O"
	};

	constexpr static size_t role_but = sizeof(role_msg[0]) - 1 + 6;

	constexpr static std::array<char, role_but+1> role_border = makeBorder<role_but+1>();
	constexpr static std::array<char, role_but+1> role_side_border = makeSideBorder<role_but+1>();

	constexpr static char back[] = "[B]ack";

	constexpr static size_t back_but = sizeof(back) - 1 + 4;
	constexpr static std::array<char, back_but+1> back_border = makeBorder<back_but+1>();

	Opponent opponent = Opponent::Player;
	Opponent last_opponent = Opponent::Player;
	uint16_t x_score = 0;
	uint16_t o_score = 0;
	uint16_t draw_score = 0;

	constexpr static const char* opponent_name[] = {
		"Player", "Normal Bot", "Hard Bot"
	};
	constexpr static const char* win_msg[] = {
		"X win", "O win"
	};

	constexpr static char score[] = "Score:";
	constexpr static char x_scr[] = "X: ";
	constexpr static char o_scr[] = "O: ";
	constexpr static char draw_scr[] = "Draw: ";

	constexpr static size_t board_width = 25;
	constexpr static size_t board_height = 13;
	constexpr static std::array<char, board_width*board_height+1> board = makeBoard<board_width, board_height>();

	constexpr static uint64_t board_win = (
			(0b000000111ULL << 0) | (0b000111000ULL << 9) |
			(0b111000000ULL << 18) | (0b001001001ULL << 27) |
			(0b010010010ULL << 36) | (0b100100100ULL << 45) |
			(0b100010001ULL << 54)
			);

	bool player_turn = false;
	uint8_t turn = 0;
	uint16_t x_state = 0;
	uint16_t o_state = 0;

	uint16_t winner = 0;

	bool bot_role = false;
	std::mt19937 gen;
	std::uniform_int_distribution<uint8_t> bot_random;

	Model() : gen(std::random_device()()), bot_random(0, 8) {}
};
// }}}

// Functions {{{
void resetBoard(Model& model) {
	model.turn = 0;
	model.player_turn = false;
	model.o_state = 0;
	model.x_state = 0;
	model.winner = 0;
	model.last_opponent = model.opponent;
}

uint16_t isWin(uint16_t state) {
	uint16_t current = 0;
	for (size_t i = 0; i < 7; i++) {
		current = (uint16_t)(Model::board_win >> (i * 9)) & 0b111111111;
		if ((state & current) == current) return current;
	}
	current = (0b001010100 << 0);
	return (((state & current) == current) ? current : 0);
}

bool checkWinner(Model& model, bool player) {
	uint16_t current = 0;
	if (player) {
		current = isWin(model.o_state);
		if (current) {
			model.winner = current | (0b11 << 9);
			return true;
		}
	} else {
		current = isWin(model.x_state);
		if (current) {
			model.winner = current | (0b10 << 9);
			return true;
		}
	}

	return false;
}

bool markBoard(Model& model, uint8_t pos) {
	if (((model.o_state >> pos) & 0b1) || ((model.x_state >> pos) & 0b1)) return false;

	if (model.player_turn) model.o_state |= (0b1 << pos);
	else model.x_state |= (0b1 << pos);

	if (model.turn >= 4) {
		if (checkWinner(model, true)) {
			model.o_score += 1;
			return true;
		}
		else if (checkWinner(model, false)) {
			model.x_score += 1;
			return true;
		}
		if (model.turn == 8) {
			model.winner = (0b100 << 9);
			model.draw_score += 1;
			return true;
		}
	}
	model.turn += 1;
	model.player_turn = !model.player_turn;
	return false;
}

int minimax(uint16_t x, uint16_t o, int depth, bool is_max, bool bot_role) {
	if (isWin(x)) return bot_role ? (depth - 10) : (10 - depth);
	else if (isWin(o)) return bot_role ? (10 - depth) : (depth - 10);
	else if ((x | o) == 0x1FF) return 0;

	if (is_max) {
		int best = std::numeric_limits<int>::min();
		for (uint8_t cell = 0; cell < 9; cell++) {
			if ((x & (1 << cell)) || (o & (1 << cell))) continue;
			uint16_t new_x = x;
			uint16_t new_o = o;
			if (bot_role) new_o |= (1 << cell);
			else new_x |= (1 << cell);
			int score = minimax(new_x, new_o, depth + 1, false, bot_role);
			best = std::max(best, score);
		}
		return best;
	} else {
		int best = std::numeric_limits<int>::max();
		for (uint8_t cell = 0; cell < 9; cell++) {
			if ((x & (1 << cell)) || (o & (1 << cell))) continue;
			uint16_t new_x = x;
			uint16_t new_o = o;
			if (bot_role) new_x |= (1 << cell);
			else new_o |= (1 << cell);
			int score = minimax(new_x, new_o, depth + 1, true, bot_role);
			best = std::min(best, score);
		}
		return best;
	}
}

uint8_t botPlay(Model& model) {
	if (model.opponent == Opponent::NormalBot) {
		uint8_t move = 0;
		do {
			move = model.bot_random(model.gen);
		} while ((model.x_state | model.o_state) & (1 << move));
		return move;
	} else if (model.opponent == Opponent::HardBot) {
		int best_score = std::numeric_limits<int>::min();
		uint8_t best_move = 0;

		for (uint8_t m = 0; m < 9; m++) {
			if (((model.x_state >> m) & 0b1) || ((model.o_state >> m) & 0b1)) continue;

			uint16_t new_x = model.x_state;
			uint16_t new_o = model.o_state;

			if (model.bot_role) new_o |= (1 << m);
			else new_x |= (1 << m);

			int score = minimax(new_x, new_o, 0, false, model.bot_role);
			if (score > best_score) {
				best_score = score;
				best_move = m;
			}
		}
		return best_move;
	}
	return 0;
}
// }}}

// State & Input {{{
enum class State : uint8_t {
	Title,
	About,
	Game,
	SelectRole,
	Result
};

struct Input {
	bool mouse;
	union {
		char c;
		struct {
			uint8_t x;
			uint8_t y;
		};
	} input;
};
// }}}

// Draw {{{
void draw(State& state, Model& model) {
	switch (state) {
		case State::Title:
			{
				size_t t_middle = getMiddle(width, sizeof(model.title));
				size_t s_middle = getMiddle(width, model.start_but);
				size_t a_middle = getMiddle(width, model.about_but);
				size_t e_middle = getMiddle(width, model.exit_but);

				std::cout
					<< "\033[1;" << t_middle << "H"
					<< model.title
					<< "\033[3;" << s_middle << "H"
					<< model.start_border.data()
					<< "\033[4;" << s_middle << "H| "
					<< model.start
					<< " |\033[5;" << s_middle << "H"
					<< model.start_border.data()
					<< "\033[7;" << a_middle << "H"
					<< model.about_border.data()
					<< "\033[8;" << a_middle << "H| "
					<< model.about
					<< " |\033[9;" << a_middle << "H"
					<< model.about_border.data()
					<< "\033[11;" << e_middle << "H"
					<< model.exit_border.data()
					<< "\033[12;" << e_middle << "H| "
					<< model.exit
					<< " |\033[13;" << e_middle << "H"
					<< model.exit_border.data();
			}
			break;
		case State::About:
			{
				size_t a_middle = getMiddle(width, model.back_but);
				size_t b_right = (model.back_but > width) ? 1 : ((size_t)width - model.back_but);
				std::cout
					<< "\033[1;" << a_middle << "H"
					<< model.about_title
					<< "\033[3H" << model.about_text
					<< "\033[" << height - 3 << ";"
					<< b_right << "H"
					<< model.back_border.data()
					<< "\033[" << height - 2 << ";"
					<< b_right << "H| "
					<< model.back
					<< " |\033[" << height - 1 << ";"
					<< b_right << "H"
					<< model.back_border.data();
			}
			break;
		case State::Game: case State::SelectRole: case State::Result:
			{
				size_t ol_space = width / 3;

				size_t ol_left = getMiddle(ol_space, sizeof(model.opponent_name[0]) - 1);
				size_t ol_middle = getMiddle(ol_space, sizeof(model.opponent_name[1]) - 1);
				size_t ol_right = getMiddle(ol_space, sizeof(model.opponent_name[2]) - 1);

				std::cout
					<< "\033[2;" << ol_left << "H"
					<< ((model.opponent == Opponent::Player) ? "\033[1;35m" : "")
					<< model.opponent_name[0] << "\033[0m"
					<< "\033[1;" << ol_space
					<< "H|\033[2;" << ol_space
					<< "H|\033[3;" << ol_space
					<< "H|\033[2;"
					<< ol_space + ol_middle << "H"
					<< ((model.opponent == Opponent::NormalBot) ? "\033[1;35m" : "")
					<< model.opponent_name[1] << "\033[0m"
					<< "\033[1;" << width - ol_space
					<< "H|\033[2;" << width - ol_space
					<< "H|\033[3;" << width - ol_space
					<< "H|\033[2;"
					<< (ol_space * 2) + ol_right << "H"
					<< ((model.opponent == Opponent::HardBot) ? "\033[1;35m" : "")
					<< model.opponent_name[2] << "\033[0m";

				size_t bv_middle = getMiddle(height, model.board_height);
				size_t b_middle = getMiddle(width, model.board_width);

				size_t b_hsz = model.board_height / 3;
				size_t b_wsz = model.board_width / 3;

				if (state == State::SelectRole) {
					size_t middle = width / 2;
					size_t r_middle = getMiddle(middle, model.role_but);
					size_t rv_middle = getMiddle(height, 5);

					for (size_t i = 0; i < 2; i++) {
						std::cout
							<< "\033[" << rv_middle << ";"
							<< r_middle + middle * i << "H"
							<< model.role_border.data()
							<< "\033[" << rv_middle + 1 << ";"
							<< r_middle + middle * i << "H"
							<< model.role_side_border.data()
							<< "\033[" << rv_middle + 2 << ";"
							<< r_middle + middle * i << "H| "
							<< model.role_msg[i]
							<< " |\033[" << rv_middle + 3 << ";"
							<< r_middle + middle * i << "H"
							<< model.role_side_border.data()
							<< "\033[" << rv_middle + 4 << ";"
							<< r_middle + middle * i << "H"
							<< model.role_border.data();
					}
				} else {
					for (size_t h = 0; h < model.board_height; h++) {
						std::cout << "\033[" << bv_middle + h << ";" << b_middle << "H";
						for (size_t w = 0; w < model.board_width; w++) {
							std::cout << model.board[model.board_width * h + w];
						}
					}

					size_t b_mhsz = getMiddle(b_hsz - 1, 1);
					size_t b_mwsz = getMiddle(b_wsz - 1, 1);

					for (uint8_t x = 0; x < 3; x++) {
						for (uint8_t y = 0; y < 3; y++) {
							if ((model.o_state >> (y * 3 + x)) & 0b1)
								std::cout
									<< "\033["
									<< bv_middle + b_mhsz + b_hsz * y
									<< ";"
									<< b_middle + b_mwsz + b_wsz * x
									<< "HO";
							else if ((model.x_state >> (y * 3 + x)) & 0b1)
								std::cout
									<< "\033["
									<< bv_middle + b_mhsz + b_hsz * y
									<< ";"
									<< b_middle + b_mwsz + b_wsz * x
									<< "HX";
						}
					}

					if (state == State::Result) {
						if ((model.winner >> 11) & 0b1) {
							for (size_t i = 0; i < 9; i++) {
								char player = ((model.x_state >> i) & 0b1) ? 'X' : ((model.o_state >> i) & 0b1) ? 'O' : 'E';
								for (size_t h = 0; h < b_hsz - 1; h++)
								{
									std::cout << "\033[" << bv_middle + 1 + h + b_hsz * (i / 3) << ";" << b_middle + 1 + b_wsz * (i % 3) << "H\033[1;37;41m";
									for (size_t w = 0; w < b_wsz - 1; w++) {
										std::cout << ((h + 1 == b_mhsz && w + 1 == b_mwsz) ? player : ' ');
									}
									std::cout << "\033[0m";
								}

							}

							std::cout << "\033[" << height - 3 << ";" << getMiddle(width, 6) << "H\033[1;31;47m Draw \033[0m";
						} else if (((model.winner >> 10) & 0b1) == 0b1) {
							char player = ' ';
							if (((model.winner >> 9) & 0b1) == 0b1) {
								player = 'O';
							} else {
								player = 'X'; 
							}

							for (size_t i = 0; i < 9; i++) {
								if (((model.winner >> i) & 0b1) == 0b1) {
									for (size_t h = 0; h < b_hsz - 1; h++)
									{
										std::cout << "\033[" << bv_middle + 1 + h + b_hsz * (i / 3) << ";" << b_middle + 1 + b_wsz * (i % 3) << "H\033[1;31;47m";
										for (size_t w = 0; w < b_wsz - 1; w++) {
											std::cout << ((h + 1 == b_mhsz && w + 1 == b_mwsz) ? player : ' ');
										}
										std::cout << "\033[0m";
									}
								}
							}

							std::cout << "\033[" << height - 3 << ";" << getMiddle(width, 7) << "HWinner:"
								<< "\033[" << height - 2 << ";" << getMiddle(width, sizeof(model.win_msg[(model.winner >> 9) & 0b1]) - 1) << "H\033[1;33m" << model.win_msg[(model.winner >> 9) & 0b1] << "\033[0m";
						}
					} else {
						std::cout
							<< "\033[" << height - 2 << ";"
							<< getMiddle(width, 7)
							<< "HTurn: " << ((model.player_turn) ? "\033[1;35mO\033[0m" : "\033[1;36mX\033[0m");
					}
				}

				size_t back_right = getRight(width, model.back_but);

				std::cout
					<< "\033[" << height - 3 << ";2H"
					<< model.score
					<< "\033[" << height - 2 << ";2H"
					<< model.x_scr << model.x_score
					<< "\033[" << height - 1 << ";2H"
					<< model.o_scr << model.o_score
					<< "\033[" << height << ";2H"
					<< model.draw_scr << model.draw_score
					<< "\033[" << height - 3 << ";"
					<< back_right << "H"
					<< model.back_border.data()
					<< "\033[" << height - 2 << ";"
					<< back_right << "H| "
					<< model.back
					<< " |\033[" << height - 1 << ";"
					<< back_right << "H"
					<< model.back_border.data();
			}
			break;
	}
}
// }}}

// Input Handling {{{
bool handleInput(State& state, Model& model, Input& input) {
	switch (state) {
		case State::Title:
			{
				if (input.mouse) {
					if (isPointInside(
								getMiddle(width, model.start_but),
								(size_t)3,
								model.start_but, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y)) {
						state = State::Game;
					}
					else if (isPointInside(
								getMiddle(width, model.about_but),
								(size_t)7,
								model.about_but, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y)) {
						state = State::About;
					}
					else if (isPointInside(
								getMiddle(width, model.exit_but),
								(size_t)11,
								model.exit_but, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y)) {
						return true;
					}
				} else {
					switch (input.input.c) {
						case 'e': case 'E':
							return true;
						case 's': case 'S':
							state = State::Game;
							break;
						default:
							break;
					}
				}
			}
			break;
		case State::About:
			{
				if (input.mouse) {
					if (isPointInside(
								getRight(width, model.back_but),
								(size_t)(height - 3),
								model.back_but, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y)) state = State::Title;
				} else {
					switch (input.input.c) {
						case 'b': case 'B':
							state = State::Title;
							break;
						default:
							break;
					}
				}
			}
			break;
		case State::Game:
			{
				if (input.mouse) {
					size_t ol_space = width / 3;
					size_t bv_middle = getMiddle(height, model.board_height);
					size_t b_middle = getMiddle(width, model.board_width);
					size_t b_wsz = model.board_width / 3 - 1;
					size_t b_hsz = model.board_height / 3 - 1;

					if (isPointInside(
								(size_t)1, (size_t)1,
								ol_space, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y) &&
							model.opponent != Opponent::Player) {
						model.opponent = Opponent::Player;
						resetBoard(model);
						model.x_score = 0;
						model.o_score = 0;
						model.draw_score = 0;
					}
					else if (isPointInside(
								ol_space, (size_t)1,
								ol_space, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y) &&
							model.opponent != Opponent::NormalBot) {
						state = State::SelectRole;
						model.opponent = Opponent::NormalBot;
					}
					else if (isPointInside(
								ol_space * 2, (size_t)1,
								ol_space, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y) &&
							model.opponent != Opponent::HardBot) {
						state = State::SelectRole;
						model.opponent = Opponent::HardBot;
					}
					else if (isPointInside(
								b_middle + 1, bv_middle + 1,
								model.board_width - 1,
								model.board_height - 3,
								(size_t)input.input.x,
								(size_t)input.input.y)) {
						for (size_t i = 0; i < 3; i++) {
							for (size_t j = 0; j < 3; j++) {
								if (isPointInside(
											b_middle + 1 + ((b_wsz + 1) * j),
											bv_middle + 1 + ((b_hsz + 1) * i),
											b_wsz - 1, b_hsz - 1,
											(size_t)input.input.x,
											(size_t)input.input.y)) {
									if (markBoard(model, i * 3 + j)) state = State::Result;
								}
							}
						}
					}
					else if (isPointInside(
								getRight(width, model.back_but),
								(size_t)(height - 3),
								model.back_but, (size_t)2,
								(size_t)input.input.x,
								(size_t)input.input.y)) {
						resetBoard(model);
						model.x_score = 0;
						model.o_score = 0;
						model.draw_score = 0;
						state = State::Title;
					}
				} else {
					switch (input.input.c) {
						case 'b': case 'B':
							resetBoard(model);
							state = State::Title;
							model.x_score = 0;
							model.o_score = 0;
							model.draw_score = 0;
							break;
						default:
							break;
					}
				}
			}
			break;
		case State::SelectRole:
			if (input.mouse) {
				if (isPointInside(
							getMiddle(width/2, model.role_but),
							getMiddle(height, (size_t)5),
							model.role_but, (size_t)4,
							(size_t)input.input.x,
							(size_t)input.input.y)) {
					model.bot_role = true;
					resetBoard(model);
					model.x_score = 0;
					model.o_score = 0;
					model.draw_score = 0;
				}
				else if(isPointInside(
							getMiddle(width/2, model.role_but) + (size_t)(width/2),
							getMiddle(height, (size_t)5),
							model.role_but, (size_t)4,
							(size_t)input.input.x,
							(size_t)input.input.y)) {
					model.bot_role = false;
					resetBoard(model);
					model.x_score = 0;
					model.o_score = 0;
					model.draw_score = 0;
				} else model.opponent = model.last_opponent;
			}
			state = State::Game;
			break;
		case State::Result:
			if (input.mouse) {
				if (isPointInside(
							getRight(width, model.back_but),
							(size_t)(height - 3),
							model.back_but, (size_t)2,
							(size_t)input.input.x,
							(size_t)input.input.y)) {
					state = State::Title;
					resetBoard(model);
					model.x_score = 0;
					model.o_score = 0;
					model.draw_score = 0;
				} else {
					state = State::Game;
					resetBoard(model);
				}
				break;
			} else {
				switch (input.input.c) {
					case 'b': case 'B':
						resetBoard(model);
						model.x_score = 0;
						model.o_score = 0;
						model.draw_score = 0;
						state = State::Title;
						break;
					default:
						resetBoard(model);
						state = State::Game;
						break;
				}
			}
			break;
	}
	return false;
}
// }}}

using Term = struct termios;
using Siga = struct sigaction;

Term getTerminal() {
	Term terminal;
	tcgetattr(STDIN_FILENO, &terminal);
	return terminal;
}

int setTerminal(Term &term, bool init) {
	if (init) term.c_lflag &= ~(ICANON | ECHO);
	else term.c_lflag |= ICANON | ECHO;
	return tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

using SigaCall = void(*)(int);
Siga createSignalHandle(SigaCall call) {
	Siga sa;

	sa.sa_handler = call;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	return sa;
}

void subscribeSignal(Siga &sa, int sigcode) {
	sigaction(sigcode, &sa, NULL);
}

template<typename T>
void getTerminalSize(T& w, T& h) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) return;
	w = ws.ws_col;
	h = ws.ws_row;
}

// Main {{{
int main() {
	Term term = getTerminal();
	if (setTerminal(term, true) != 0) return -1;
	Siga sighandle = createSignalHandle(signalHandle);
	subscribeSignal(sighandle, SIGWINCH);
	getTerminalSize<uint16_t>(width, height);

	int epoll_fd = epoll_create1(0);
	if (epoll_fd == - 1) {
		std::cerr << "Unable to create epoll instance!" << std::endl;
		setTerminal(term, false);
		return -1;
	}

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.fd = STDIN_FILENO;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &event) == -1) {
		std::cerr << "Unable to connect epoll to stdin!" << std::endl;
		close(epoll_fd);
		setTerminal(term, false);
		return -1;
	}

	std::cout << "\033c\033[?25l\033[?1000h\033[?1002h";
	std::cout.flush();
	/* ======= START OF MAIN ======= */
	{
		std::array<epoll_event, 1> input_event;

		Model model;
		State state = State::Title;
		Input input{false, ' '};

		bool run = true;

		draw(state, model);
		std::cout.flush();
		while (run) {
			if (model.opponent != Opponent::Player && model.player_turn == model.bot_role) {
				std::cout << "\033[H\033[2J";
				if (markBoard(model, botPlay(model))) {
					model.player_turn = !model.bot_role;
					state = State::Result;
				}
				draw(state, model);
				std::cout.flush();
				continue;
			}

			if (epoll_wait(epoll_fd, input_event.data(), 1, 150) <= 0) {
				if (terminal_resized) {
					getTerminalSize<uint16_t>(width, height);
					std::cout << "\033[H\033[2J";
					draw(state, model);
					std::cout.flush();
					terminal_resized = 0;
				}
				continue;
			}

			if (input_event[0].data.fd == STDIN_FILENO) {
				char buf[6] = {' ', ' ', ' ', ' ', ' '};
				if (read(STDIN_FILENO, buf, sizeof(buf)) > 0) {
					std::cout << "\033[H\033[2J";

					if (buf[0] == '\x1b' && buf[1] == '[') {
						if (buf[2] == 'm' || buf[2] == 'M') {
							input.mouse = true;
							if (buf[3] - 32 != 3) continue;
							input.input.x = (uint8_t)buf[4] - 32;
							input.input.y = (uint8_t)buf[5] - 32;
						}
					} else {
						input.mouse = false;
						input.input.c = buf[0];
					}
				}
				run = !handleInput(state, model, input);
				draw(state, model);
				std::cout.flush();
			}
		}
	}
	/* =======  END  OF MAIN ======= */
	std::cout << "\033c";
	std::cout.flush();
	close(epoll_fd);
	setTerminal(term, false);
	return 0;
}
// }}}