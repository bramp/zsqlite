#include <snappy.h>

#include <string>
#include <iostream>
#include <iterator>
#include <fstream>
#include <vector>

#include <stdint.h>

using namespace std;
using namespace snappy;


// function copied from snappy
inline char* string_as_array(string * str) {
	return str->empty() ? NULL : &*str->begin();
}

struct header {
	int block_size;
	int index_len; // 16 Terabytes (compressed)

	header(int block_size, int index_len)
		: block_size(block_size), index_len(index_len) {}

	friend ostream& operator<< (ostream &, const struct header &);
};

ostream& operator<< (ostream &out, const struct header &head) {
	return out.write( (const char *)&head, 8 );
}


int main(int argc, char *argv[]) {
	const size_t block_size = 4096;

	ifstream in_file;
	in_file.open ("/home/bramp/personal/map/acs/acs2010_5yr/master.sqlite", ios::binary | ios::in);
	if (!in_file.is_open()) {
		cerr << "Failed to open input file" << endl;
		return -1;
	}

	ofstream out_file;
	out_file.open ("snappy-sqlite.cc.snap", ios::binary | ios::out);
	if (!out_file.is_open()) {
		cerr << "Failed to open output file" << endl;
		return -1;
	}


	in_file.seekg (0, ios::end);
	int index_len = in_file.tellg() / block_size + 1;

	header head(block_size, index_len);
	vector< uint16_t > index;

	index.reserve(index_len);

	string uncompressed( block_size, '\0' );
	string compressed( MaxCompressedLength(block_size), '\0' );

	long long in_total = 0, out_total = 0;

	in_file.seekg(0);

	int index_bytes = index_len * sizeof(uint16_t);
	out_file.seekp(index_bytes + sizeof(head));

	while (!in_file.eof()) {
		in_file.read(string_as_array(&uncompressed), uncompressed.size());
		size_t in_len = in_file.gcount();
		in_total += in_len;

		Compress(uncompressed.data(), in_len, &compressed);

		// write compressed to file
		out_file.write(compressed.data(), compressed.size());
		out_total += compressed.size();

		index.push_back(compressed.size());
	}

	in_file.close();

	// Seek to the beginning of the file and write the header / index
	out_file.seekp(0);

	//assert(sizeof(header) == 8);
	out_file.write( (const char *)&head, sizeof(head));
	out_file.write( (const char *)&index[0], index_len * sizeof(&index[0]) );
	//for (vector<uint16_t>::iterator it = index.begin(); it != index.end(); it++)
	//	out_file.write( (const char *)*it, sizeof(uint16_t) );

	out_file.close();

	cout << "Uncompressed: " << (in_total / 1024) << " KiB " << endl
	     << "  Compressed: " << (out_total / 1024) << " KiB + "
	     << "Index: " << (index_bytes / 1024) << " KiB " << endl
	     << "       Ratio: " << ((float)(out_total + index_bytes) / (float)in_total)
	     << endl;

	return 0;
}
