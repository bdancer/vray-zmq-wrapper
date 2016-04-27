#ifndef _SERIALIZER_HPP_
#define _SERIALIZER_HPP_

#include <vector>
#include <string>
#include "base_types.h"

class SerializerStream {
public:

	SerializerStream() {
	}

	void write(const char * data, size_t size) {
		if (size == 0) {
			return;
		}
		size_t prevSize = stream.size();
		stream.resize(size + stream.size());
		memcpy(&stream[prevSize], data, size);
	}

	size_t getSize() const {
		return stream.size();
	}

	char * getData() {
		return stream.data();
	}

private:
	std::vector<char> stream;
};


template <typename T>
inline SerializerStream & operator<<(SerializerStream & stream, const T & value) {
	stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
	return stream;
}

template <>
inline SerializerStream & operator<<(SerializerStream & stream, const std::string & value) {
	size_t size = value.size();
	stream << size;
	stream.write(value.c_str(), size);
	return stream;
}

template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrSimpleType<std::string> & value) {
	stream << value.m_Value;
	return stream;
}


template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrPlugin & plugin) {
	return stream << plugin.plugin << plugin.output;
}

template <typename Q>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrList<Q> & list) {
	stream << list.getCount();
	stream.write(reinterpret_cast<const char *>(list.getData()->data()), list.getCount() * sizeof(Q));
	return stream;
}

// specialization for AttrListString and AttrListPlugin, as it is not continious block of memory
template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrList<VRayBaseTypes::AttrPlugin> & list) {
	stream << list.getCount();
	if (!list.empty()) {
		for (auto & item : *(list.getData())) {
			stream << item;
		}
	}
	return stream;
}

template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrList<std::string> & list) {
	stream << list.getCount();
	if (!list.empty()) {
		for (auto & item : *(list.getData())) {
			stream << item;
		}
	}
	return stream;
}



template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrMapChannels & map) {
	stream << static_cast<int>(map.data.size());
	for (auto & pair : map.data) {
		stream << pair.first << pair.second.vertices << pair.second.faces << pair.second.name;
	}
	return stream;
}


template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrInstancer::Item & instItem) {
	return stream << instItem.index << instItem.tm << instItem.vel << instItem.node;
}

template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrInstancer & inst) {
	stream << inst.frameNumber << inst.data.getCount();
	if (!inst.data.empty()) {
		for (auto & item : *(inst.data.getData())) {
			stream << item;
		}
	}
	return stream;
}


template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrImage & image) {
	stream << image.imageType << image.size << image.width << image.height << image.x << image.y;
	stream.write(image.data.get(), image.size);
	return stream;
}

template <>
inline SerializerStream & operator<<(SerializerStream & stream, const VRayBaseTypes::AttrImageSet & set) {
	stream << set.sourceType << static_cast<int>(set.images.size());
	for (const auto &img : set.images) {
		stream << img.first << img.second;
	}
	return stream;
}

#endif // _SERIALIZER_HPP_