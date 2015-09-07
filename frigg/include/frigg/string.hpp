
namespace frigg {
namespace util {

template<typename Char>
class BasicStringView {
public:
	typedef Char CharType;

	BasicStringView(const Char *c_string)
	: p_pointer(c_string), p_length(strlen(c_string)) { }

	BasicStringView(const Char *pointer, size_t length)
	: p_pointer(pointer), p_length(length) { }

	template<typename String>
	bool operator== (const String &other) const {
		if(p_length != other.size())
			return false;
		for(size_t i = 0; i < p_length; i++)
			if(p_pointer[i] != other.data()[i])
				return false;
		return true;
	}

	const Char *data() const {
		return p_pointer;
	}

	size_t size() const {
		return p_length;
	}

private:
	const Char *p_pointer;
	size_t p_length;
};

typedef BasicStringView<char> StringView;

template<typename Char, typename Allocator>
class BasicString {
public:
	typedef Char CharType;

	BasicString(Allocator &allocator)
	: p_allocator(allocator), p_buffer(nullptr), p_length(0) { }

	BasicString(Allocator &allocator, const Char *c_string)
	: p_allocator(allocator) {
		p_length = strlen(c_string);
		p_buffer = (Char *)p_allocator.allocate(sizeof(Char) * p_length);
		memcpy(p_buffer, c_string, sizeof(Char) * p_length);
	}
	
	// TODO: ensure that both CharTypes are the same
	template<typename String>
	BasicString &operator+= (const String &other) {
		size_t new_length = p_length + other.size();
		Char *new_buffer = (Char *)p_allocator.allocate(sizeof(Char) * new_length);
		memcpy(new_buffer, p_buffer, sizeof(Char) * p_length);
		memcpy(new_buffer + p_length, other.data(), sizeof(Char) * other.size());
		
		if(p_buffer != nullptr)
			p_allocator.free(p_buffer);
		p_length = new_length;
		p_buffer = new_buffer;

		return *this;
	}

	const Char *data() const {
		return p_buffer;
	}

	size_t size() const {
		return p_length;
	}

private:
	Allocator &p_allocator;
	Char *p_buffer;
	size_t p_length;
};

template<typename Allocator>
using String = BasicString<char, Allocator>;

} } // namespace frigg::util

namespace frigg {
namespace debug {

template<typename P>
struct Print<P, util::StringView> {
	static void print(P &printer, util::StringView string) {
		for(size_t i = 0; i < string.size(); i++)
			printer.print(string.data()[i]);
	}
};

} } // namespace frigg::debug

