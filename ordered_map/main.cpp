#include "parse.h"
#include "jade_latency.h"

int main() {
	Engine engine;
	parser data_parser(engine);
	
	jade::bench("Warm bench", 10000, [&] {data_parser.getMessage();}, jade::Warm{});
	jade::bench("Cold bench", 10000, [&] {data_parser.getMessage();}, jade::Cold{});
	jade::report(stderr);
	jade::write_csv("latency.csv");
	jade::write_summary_csv("summary.csv");
	return 0;
}