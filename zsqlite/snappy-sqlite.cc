#include <string>
#include <iostream>
#include <iterator>
#include <fstream>
#include <vector>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <assert.h>
#include <stdint.h>

#include <snappy.h>

#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>

using namespace std;
using namespace snappy;


// function copied from snappy
inline char* string_as_array(std::string * str) {
	return str->empty() ? NULL : &*str->begin();
}

inline const char* string_as_array(const std::string * str) {
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

class SnappyCompressor {

public:
	SnappyCompressor() {}

	void compress(const std::string & in, std::string & out) {
		Compress(in.data(), in.size(), &out);

		#ifdef PARANOID
		assert( IsValidCompressedBuffer(string_as_array(&out), out.size()) );
		#endif
	}
};

/**
 * TODO Test LZO1F
 */
class LZOCompressor {

	char * wrkmem;

public:
	LZOCompressor() {
		if (lzo_init() != LZO_E_OK) {
			cerr << "Failed to init LZO" << endl;
			throw new runtime_error("Failed to init LZO");
		}

		this->wrkmem = new char[LZO1X_1_MEM_COMPRESS];
	}

	~LZOCompressor() {
		delete[] this->wrkmem;
	}

	void compress(const std::string & in, std::string & out) {
		size_t in_len = in.size();
		size_t out_len = in_len + in_len / 16 + 64 + 3;
		out.reserve(out_len);

		int r = lzo1x_1_compress(
			(unsigned char *) string_as_array(&in), in_len,
			(unsigned char *) string_as_array(&out), &out_len, wrkmem);
		if (r != LZO_E_OK) {
			printf("internal error - compression failed: %d\n", r);
		}

		out.resize(out_len);
	}
};

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

	LZOCompressor * compressor = new LZOCompressor();
	//SnappyCompressor * compressor = new SnappyCompressor();

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
		uncompressed.resize(in_len);
		in_total += in_len;

		assert(in_len > 0);

		compressor->compress(uncompressed, compressed);

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
