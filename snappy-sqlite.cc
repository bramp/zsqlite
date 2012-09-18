#include <snappy.h>

#include <string>
#include <iostream>
#include <iterator>
#include <fstream>
#include <vector>
#include <cerrno>
#include <cstring>

#include <assert.h>
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

streampos file_len(ifstream &s) {
	s.seekg (0, ios::end);
	return s.tellg();
}

int main(int argc, const char *argv[]) {
	const size_t block_size = 4096;

	if (argc != 3) {
		cerr << "Usage: " << argv[0] << " {source} {dest}" << endl;
		return -1;
	}

	const char * src = argv[1];
	const char * dst = argv[2];

	ifstream in_file (src, ios::binary | ios::in);
	if (!in_file) {
		cerr << "Failed to open source file: " << src << endl;
		return -1;
	}
//	in_file.exceptions(ios::badbit | ios::failbit);


	ofstream out_file(dst, ios::binary | ios::out);
	if (!out_file) {
		cerr << "Failed to open output file: " << dst << endl;
		return -1;
	}
//	out_file.exceptions(ios::badbit | ios::failbit);

	int index_len = file_len(in_file) / block_size + 1;

	header head(block_size, index_len);
	vector< uint16_t > index;

	index.reserve(index_len);

	string uncompressed( block_size, '\0' );
	string compressed( MaxCompressedLength(block_size), '\0' );

	long long in_total = 0, out_total = 0;

	in_file.seekg(0, ios_base::beg);

	int index_bytes = index_len * sizeof(uint16_t);
	int data_start  = index_bytes + sizeof(head);
	out_file.seekp(data_start, ios_base::beg);

	while (in_file.good()) {
		in_file.read(string_as_array(&uncompressed), uncompressed.size());
		if (in_file.bad()) {
			cerr << "Error while reading source " << in_file.rdstate() << endl;
			return -1;
		}

		size_t in_len = in_file.gcount();
		in_total += in_len;

		Compress(uncompressed.data(), in_len, &compressed);
		assert( IsValidCompressedBuffer(string_as_array(&compressed), compressed.size()) );

		// write compressed to file
		out_file.write(compressed.data(), compressed.size());
		if (out_file.bad()) {
			cerr << "Error while writing to destination" << endl;
			return -1;
		}

		out_total += compressed.size();
		index.push_back(compressed.size()); // Store the size of this block
	}

	assert(index.size() > 0);
	assert(index.size() == index_len);
	in_file.close();

	// Seek to the beginning of the file and write the header / index
	out_file.clear();
	out_file.seekp(0, ios_base::beg);
	out_file.write( reinterpret_cast<char*>(&head), sizeof(head));
	out_file.write( reinterpret_cast<char*>(&index[0]), index_len * sizeof(index[0]) );

	if (out_file.bad()) {
		cerr << "Error while writing index to destination: " << strerror(errno) << endl;
		return -1;
	}

	assert( out_file.tellp() == data_start );

	out_file.close();

	cout << "Uncompressed: " << (in_total / 1024) << " KiB " << endl
	     << "  Compressed: " << (out_total / 1024) << " KiB + "
	     << "Index: " << (index_bytes / 1024) << " KiB " << endl
	     << "       Ratio: x" << ((float)in_total / (float)(out_total + index_bytes))
	     << endl;

	return 0;
}
