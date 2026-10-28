#include "catch.hpp"

#include "utils.hpp"

#include <osmium/builder/attr.hpp>
#include <osmium/io/detail/pbf_decoder.hpp>
#include <osmium/io/detail/pbf_input_format.hpp>
#include <osmium/io/detail/pbf_output_format.hpp>
#include <osmium/io/detail/read_write.hpp>
#include <osmium/io/opl_output.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/io/pbf_output.hpp>
#include <osmium/io/reader.hpp>
#include <osmium/io/writer.hpp>
#include <osmium/memory/buffer.hpp>
#include <osmium/osm/item_type.hpp>
#include <osmium/osm/object.hpp>
#include <osmium/osm/timestamp.hpp>
#include <osmium/util/file.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("Get supported PBF compression types") {
    const auto types = osmium::io::supported_pbf_compression_types();
    REQUIRE(types.size() >= 2);
    REQUIRE(types[0] == "none");
    REQUIRE(types[1] == "zlib");
}

/**
 * Osmosis writes PBF with changeset=-1 if its input file did not contain the changeset field.
 * The default value of the version field is -1 in the OSM.PBF format.
 *
 * t/io/data_pbf_version-1.osm.pbf was created using the following steps:
 *
 * * Convert t/io/data_pbf_version-1.osm to PBF format using Osmium Tool:
 *   `osmium cat -o tmp.osm.pbf --output-format pbf,add_metadata=no /data_pbf_version-1.osm.pbf`)
 * * Convert that file using Osmosis:
 *   `osmosis --read-pbf file=tmp.osm.pbf --write-pbf file=data_pbf_version-1.osm.pbf compress=none usedense=false`
 */
TEST_CASE("Read PBF file with version=-1 and changeset=-1 (written by Osmosis)") {
    const osmium::memory::Buffer buffer = osmium::io::read_file(with_data_dir("t/io/data_pbf_version-1.osm.pbf"));
    // one node should be in this buffer
    const osmium::OSMObject& object = *(buffer.cbegin<osmium::OSMObject>());
    REQUIRE(object.version() == 0);
    REQUIRE(object.changeset() == 0);
}

/**
 * Osmosis writes PBF with changeset=-1 if its input file did not contain the changeset field.
 * The default value of the version field is -1 in the OSM.PBF format.
 *
 * t/io/data_pbf_version-1.osm.pbf was created using the following steps:
 *
 * * Convert t/io/data_pbf_version-1.osm to PBF format using Osmium Tool:
 *   `osmium cat -o tmp.osm.pbf --output-format pbf,add_metadata=no /data_pbf_version-1.osm.pbf`)
 * * Convert that file using Osmosis:
 *   `osmosis --read-pbf file=tmp.osm.pbf --write-pbf file=data_pbf_version-1-densenodes.osm.pbf compress=none usedense=true`
 */
TEST_CASE("Read PBF file with version=-1 and changeset=-1 and DenseNodes (written by Osmosis)") {
    const osmium::memory::Buffer buffer = osmium::io::read_file(with_data_dir("t/io/data_pbf_version-1-densenodes.osm.pbf"));
    // one node should be in this buffer
    const osmium::OSMObject& object = *(buffer.cbegin<osmium::OSMObject>());
    REQUIRE(object.version() == 0);
    REQUIRE(object.changeset() == 0);
}

TEST_CASE("get size in network byte order") {
    const std::array<char, 4> data1 = { 0, 0, 0, 1 };
    REQUIRE(osmium::io::detail::PBFParser::get_size_in_network_byte_order(data1.data()) == 1);

    const std::array<char, 4> data127 = { 0, 0, 0, 127 };
    REQUIRE(osmium::io::detail::PBFParser::get_size_in_network_byte_order(data127.data()) == 127);

    const std::array<char, 4> data128 = { 0, 0, 0, static_cast<char>(128) };
    REQUIRE(osmium::io::detail::PBFParser::get_size_in_network_byte_order(data128.data()) == 128);

    const std::array<char, 4> data65535 = { 0, 0, static_cast<char>(255), static_cast<char>(255) };
    REQUIRE(osmium::io::detail::PBFParser::get_size_in_network_byte_order(data65535.data()) == 65535);
}

namespace {

/**
 * Write a PBF file with more nodes than fit into one block. It will contain
 * four data blobs: three with 8000, 8000, and 4000 nodes and one with
 * 10 ways.
 */
void write_multi_blob_file(const char* filename) {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 20000; ++id) {
        osmium::builder::add_node(buffer, _id(id), _version(1), _location(1.0, 2.0));
    }
    for (osmium::object_id_type id = 1; id <= 10; ++id) {
        osmium::builder::add_way(buffer, _id(id), _version(1), _nodes({1, 2}));
    }

    osmium::io::Writer writer{filename, osmium::io::overwrite::allow};
    writer(std::move(buffer));
    writer.close();
}

std::vector<std::string> read_data_blobs(const char* filename) {
    const int fd = osmium::io::detail::open_for_reading(filename);
    REQUIRE_FALSE(osmium::io::detail::read_pbf_blob(fd, "OSMHeader").empty());

    std::vector<std::string> blobs;
    for (std::string blob = osmium::io::detail::read_pbf_blob(fd, "OSMData");
         !blob.empty();
         blob = osmium::io::detail::read_pbf_blob(fd, "OSMData")) {
        blobs.push_back(std::move(blob));
    }

    osmium::io::detail::reliable_close(fd);
    return blobs;
}

std::vector<std::string> read_object_ids(const char* filename) {
    std::vector<std::string> ids;
    osmium::io::Reader reader{filename};
    while (const auto buffer = reader.read()) {
        for (const auto& object : buffer.select<osmium::OSMObject>()) {
            ids.push_back(osmium::item_type_to_char(object.type()) + std::to_string(object.id()));
        }
    }
    reader.close();
    return ids;
}

} // anonymous namespace

TEST_CASE("Read data blobs from PBF file") {
    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");
    const auto blobs = read_data_blobs("test-pbf-blobs-in.osm.pbf");
    REQUIRE(blobs.size() == 4);
}

TEST_CASE("Reading PBF blob with wrong type throws") {
    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");
    const int fd = osmium::io::detail::open_for_reading("test-pbf-blobs-in.osm.pbf");
    REQUIRE_THROWS_AS(osmium::io::detail::read_pbf_blob(fd, "OSMData"), osmium::pbf_error);
    osmium::io::detail::reliable_close(fd);
}

TEST_CASE("Reading truncated PBF blob throws") {
    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");
    const auto size = osmium::file_size("test-pbf-blobs-in.osm.pbf");

    const int fd = osmium::io::detail::open_for_reading("test-pbf-blobs-in.osm.pbf");
    std::string data(size - 10, '\0');
    REQUIRE(osmium::io::detail::read_exactly(fd, data.data(), static_cast<unsigned int>(data.size())));
    osmium::io::detail::reliable_close(fd);

    const int out = osmium::io::detail::open_for_writing("test-pbf-blobs-truncated.osm.pbf", osmium::io::overwrite::allow);
    osmium::io::detail::reliable_write(out, data.data(), data.size());
    osmium::io::detail::reliable_close(out);

    REQUIRE_THROWS_AS(read_data_blobs("test-pbf-blobs-truncated.osm.pbf"), osmium::pbf_error);
}

TEST_CASE("Reading PBF blob with BlobHeader size zero is treated as EOF like in PBFParser") {
    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");
    const auto size = osmium::file_size("test-pbf-blobs-in.osm.pbf");

    const int fd = osmium::io::detail::open_for_reading("test-pbf-blobs-in.osm.pbf");
    std::string data(size, '\0');
    REQUIRE(osmium::io::detail::read_exactly(fd, data.data(), static_cast<unsigned int>(data.size())));
    osmium::io::detail::reliable_close(fd);

    data.append(4, '\0');
    const int out = osmium::io::detail::open_for_writing("test-pbf-blobs-padded.osm.pbf", osmium::io::overwrite::allow);
    osmium::io::detail::reliable_write(out, data.data(), data.size());
    osmium::io::detail::reliable_close(out);

    REQUIRE(read_data_blobs("test-pbf-blobs-padded.osm.pbf").size() == 4);
    REQUIRE(read_object_ids("test-pbf-blobs-padded.osm.pbf") == read_object_ids("test-pbf-blobs-in.osm.pbf"));
}

TEST_CASE("Copy PBF blobs mixed with objects to new PBF file") {
    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");
    auto blobs = read_data_blobs("test-pbf-blobs-in.osm.pbf");
    REQUIRE(blobs.size() == 4);

    // The size of the output queue must not change the order of blobs.
    osmium::io::output_queue_size queue_size;
    SECTION("default output queue size") {
    }
    SECTION("output queue size 1") {
        queue_size = osmium::io::output_queue_size{1};
    }
    SECTION("output queue size 1000") {
        queue_size = osmium::io::output_queue_size{1000};
    }

    {
        osmium::io::Writer writer{"test-pbf-blobs-out.osm.pbf", osmium::io::overwrite::allow, queue_size};
        writer.write_pbf_blob(std::move(blobs[0]));

        // Objects of the second blob are written as normal objects, they
        // must end up between the first and the third blob.
        osmium::io::detail::PBFDataBlobDecoder decoder{std::move(blobs[1]), osmium::osm_entity_bits::all, osmium::io::read_meta::yes};
        osmium::memory::Buffer buffer = decoder();
        while (buffer.has_nested_buffers()) {
            const auto nested = buffer.get_last_nested();
            for (const auto& object : nested->select<osmium::OSMObject>()) {
                writer(object);
            }
        }
        for (const auto& object : buffer.select<osmium::OSMObject>()) {
            writer(object);
        }

        writer.write_pbf_blob(std::move(blobs[2]));
        writer.write_pbf_blob(std::move(blobs[3]));
        writer.close();
    }

    REQUIRE(read_object_ids("test-pbf-blobs-out.osm.pbf") == read_object_ids("test-pbf-blobs-in.osm.pbf"));
    REQUIRE(read_data_blobs("test-pbf-blobs-out.osm.pbf").size() == 4);
}

namespace {

/**
 * Write 20000 nodes, 9000 ways and 3 relations with tags and metadata,
 * one by one through the internal buffer of the writer with the specified
 * size, so that blocks are filled from several buffers.
 */
void write_mixed_file(const char* filename, std::size_t writer_buffer_size) {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 20000; ++id) {
        osmium::builder::add_node(buffer, _id(id), _version(2), _cid(id + 100), _uid(id % 7), _user(id % 2 ? "foo" : "bar"),
                                  _timestamp(osmium::Timestamp{"2020-01-01T00:00:00Z"}), _location(1.0 + 0.0001 * static_cast<double>(id), 2.0),
                                  _tag("n", std::to_string(id % 13)));
    }
    for (osmium::object_id_type id = 1; id <= 9000; ++id) {
        osmium::builder::add_way(buffer, _id(id), _version(1), _user("foo"), _nodes({id, id + 1}), _tag("highway", "road"));
    }
    for (osmium::object_id_type id = 1; id <= 3; ++id) {
        osmium::builder::add_relation(buffer, _id(id), _version(1), _member(osmium::item_type::way, id, "outer"));
    }

    osmium::io::Header header;
    header.set("generator", "test");
    osmium::io::Writer writer{filename, header, osmium::io::overwrite::allow};
    writer.set_buffer_size(writer_buffer_size);
    for (const auto& object : buffer.select<osmium::OSMObject>()) {
        writer(object);
    }
    writer.close();
}

std::size_t count_objects(const osmium::memory::Buffer& buffer) {
    std::size_t count = 0;
    for (const auto& object : buffer.select<osmium::OSMObject>()) {
        (void)object;
        ++count;
    }
    return count;
}

std::vector<std::size_t> count_objects_per_blob(const char* filename) {
    std::vector<std::size_t> counts;
    for (auto& blob : read_data_blobs(filename)) {
        osmium::io::detail::PBFDataBlobDecoder decoder{std::move(blob), osmium::osm_entity_bits::all, osmium::io::read_meta::yes};
        osmium::memory::Buffer buffer = decoder();
        std::size_t count = 0;
        while (buffer.has_nested_buffers()) {
            count += count_objects(*buffer.get_last_nested());
        }
        counts.push_back(count + count_objects(buffer));
    }
    return counts;
}

std::string read_file(const char* filename) {
    const int fd = osmium::io::detail::open_for_reading(filename);
    std::string data(osmium::file_size(filename), '\0');
    REQUIRE(osmium::io::detail::read_exactly(fd, data.data(), static_cast<unsigned int>(data.size())));
    osmium::io::detail::reliable_close(fd);
    return data;
}

} // anonymous namespace

TEST_CASE("Write PBF blocks filled from several buffers") {
    // Small writer buffer: blocks are filled from many buffers.
    write_mixed_file("test-pbf-blocks-small.osm.pbf", 64UL * 1024UL);
    // Large writer buffer: all objects are in one buffer.
    write_mixed_file("test-pbf-blocks-large.osm.pbf", 64UL * 1024UL * 1024UL);

    const std::vector<std::size_t> expected{8000, 8000, 4000, 8000, 1000, 3};
    REQUIRE(count_objects_per_blob("test-pbf-blocks-small.osm.pbf") == expected);

    const auto ids = read_object_ids("test-pbf-blocks-small.osm.pbf");
    REQUIRE(ids.size() == 29003);
    REQUIRE(ids.front() == "n1");
    REQUIRE(ids[20000] == "w1");
    REQUIRE(ids.back() == "r3");

    REQUIRE(read_file("test-pbf-blocks-small.osm.pbf") == read_file("test-pbf-blocks-large.osm.pbf"));
}

TEST_CASE("Encode objects into PBF blobs and write them") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    write_multi_blob_file("test-pbf-blobs-in.osm.pbf");

    // The same objects as in the file, split over two buffers.
    osmium::memory::Buffer nodes{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 20000; ++id) {
        osmium::builder::add_node(nodes, _id(id), _version(1), _location(1.0, 2.0));
    }
    osmium::memory::Buffer ways{1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 10; ++id) {
        osmium::builder::add_way(ways, _id(id), _version(1), _nodes({1, 2}));
    }

    auto middle = nodes.cbegin<osmium::OSMObject>();
    std::advance(middle, 12345);
    const std::vector<osmium::io::detail::pbf_item_range> ranges{
        {nodes.data(), middle.data()},
        {middle.data(), nodes.data() + nodes.committed()},
        {ways.data(), ways.data() + ways.committed()}};

    const osmium::io::File file{"test-pbf-blobs-out.osm.pbf"};
    std::size_t bucket_count = 1;
    auto blobs = osmium::io::detail::encode_pbf_data_blobs(ranges, osmium::io::detail::make_pbf_output_options(file), bucket_count);
    REQUIRE(blobs.size() == 4);

    osmium::io::Writer writer{file, osmium::io::overwrite::allow};
    for (auto& blob : blobs) {
        writer.write_pbf_blob(std::move(blob));
    }
    writer.close();

    REQUIRE(read_file("test-pbf-blobs-out.osm.pbf") == read_file("test-pbf-blobs-in.osm.pbf"));
}

TEST_CASE("Invalid PBF output options throw") {
    const osmium::io::File file{"test.osm.pbf", "pbf,pbf_compression_level=foo"};
    REQUIRE_THROWS_AS(osmium::io::detail::make_pbf_output_options(file), std::invalid_argument);
}

TEST_CASE("Decode first object of PBF block from beginning of its data") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    std::string format{"pbf"};
    SECTION("zlib") {
    }
#ifdef OSMIUM_WITH_LZ4
    SECTION("lz4") {
        format = "pbf,pbf_compression=lz4";
    }
#endif
    SECTION("no compression") {
        format = "pbf,pbf_compression=none";
    }
    SECTION("no dense nodes") {
        format = "pbf,pbf_dense_nodes=false";
    }

    osmium::memory::Buffer buffer{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_node(buffer, _id(-7), _version(1), _user("foo"), _location(1.0, 2.0), _tag("name", "x"));
    for (osmium::object_id_type id = 1; id <= 10000; ++id) {
        osmium::builder::add_node(buffer, _id(id), _version(1), _user("foo"), _location(1.0, 2.0), _tag("name", std::to_string(id)));
    }
    for (osmium::object_id_type id = 1; id <= 10; ++id) {
        osmium::builder::add_way(buffer, _id(id), _version(1), _nodes({1, 2}));
    }
    osmium::builder::add_relation(buffer, _id(5), _version(1), _member(osmium::item_type::way, 1, "outer"));
    osmium::builder::add_relation(buffer, _id(6), _version(1), _member(osmium::item_type::way, 2, "outer"));

    const osmium::io::File file{"test-pbf-first-object.osm.pbf", format};
    osmium::io::Writer writer{file, osmium::io::overwrite::allow};
    writer(std::move(buffer));
    writer.close();

    const std::vector<std::pair<osmium::item_type, osmium::object_id_type>> expected{
        {osmium::item_type::node, -7},
        {osmium::item_type::node, 8000},
        {osmium::item_type::way, 1},
        {osmium::item_type::relation, 5}};

    const auto blobs = read_data_blobs("test-pbf-first-object.osm.pbf");
    REQUIRE(blobs.size() == expected.size());

    for (std::size_t i = 0; i < blobs.size(); ++i) {
        std::string full;
        const auto full_data = osmium::io::detail::decode_blob(blobs[i], full);
        REQUIRE(osmium::io::detail::decode_first_object_key(full_data) == expected[i]);

        for (const uint64_t max_size : {1U, 10U, 100U, 1000U, 10000U, 100000U}) {
            std::string output;
            const auto data = osmium::io::detail::decode_blob(blobs[i], output, max_size);
            REQUIRE(std::string(data.data(), data.size()) == std::string(full_data.data(), std::min(data.size(), full_data.size())));
            const auto key = osmium::io::detail::decode_first_object_key(data);
            if (data.size() < max_size) {
                REQUIRE(data.size() == full_data.size());
            }
            if (key.first != osmium::item_type::undefined) {
                REQUIRE(key == expected[i]);
            }
        }
    }
}

TEST_CASE("Decode first object of empty or cut off data") {
    REQUIRE(osmium::io::detail::decode_first_object_key(osmium::io::detail::data_view{}).first == osmium::item_type::undefined);

    // PrimitiveBlock with a PrimitiveGroup with Way 17, cut off after the
    // varint of the ID started.
    const std::string data{"\x12\x06\x1a\x04\x08\x91\x01\x10", 8};
    REQUIRE(osmium::io::detail::decode_first_object_key(osmium::io::detail::data_view{data.data(), 6}).first == osmium::item_type::undefined);
    REQUIRE(osmium::io::detail::decode_first_object_key(osmium::io::detail::data_view{data.data(), 7}) == std::make_pair(osmium::item_type::way, osmium::object_id_type{145}));
}

namespace {

// Encode the objects in the buffer into one PrimitiveBlock and return its
// uncompressed data.
std::string encode_block(const osmium::memory::Buffer& buffer, const char* format = "pbf,pbf_compression=none") {
    const osmium::io::File file{"test.osm.pbf", format};
    const std::vector<osmium::io::detail::pbf_item_range> ranges{{buffer.data(), buffer.data() + buffer.committed()}};
    std::size_t bucket_count = 1;
    const auto blobs = osmium::io::detail::encode_pbf_data_blobs(ranges, osmium::io::detail::make_pbf_output_options(file), bucket_count);
    REQUIRE(blobs.size() == 1);
    std::string output;
    const auto data = osmium::io::detail::decode_blob(blobs.front(), output);
    return std::string{data.data(), data.size()};
}

osmium::memory::Buffer decode_block(const std::string& data) {
    osmium::io::detail::PBFPrimitiveBlockDecoder decoder{
        osmium::io::detail::data_view{data.data(), data.size()},
        osmium::osm_entity_bits::object,
        osmium::io::read_meta::yes};
    return decoder();
}

std::vector<osmium::io::detail::pbf_encoded_object> encoded_objects(const osmium::io::detail::PBFEncodedPrimitiveBlock& block) {
    std::vector<osmium::io::detail::pbf_encoded_object> objects;
    block.for_each_object([&](const osmium::io::detail::pbf_encoded_object& object) {
        objects.push_back(object);
    });
    return objects;
}

std::vector<uint32_t> string_indexes(const osmium::io::detail::PBFEncodedPrimitiveBlock& block, const osmium::io::detail::pbf_encoded_object& object) {
    std::vector<uint32_t> indexes;
    block.for_each_string_index(object, [&](uint32_t index) {
        indexes.push_back(index);
    });
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

} // anonymous namespace

TEST_CASE("Access encoded objects of a PBF block") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_way(buffer, _id(-3), _version(2), _timestamp(osmium::Timestamp{"2020-01-01T00:00:00Z"}), _user("foo"), _nodes({1, 2}), _tag("highway", "primary"));
    osmium::builder::add_way(buffer, _id(7), _version(5), _nodes({1, 2, 3}));
    osmium::builder::add_way(buffer, _id(8), _version(1), _user("bar"), _nodes({3, 4}), _tag("highway", "residential"));
    const auto data = encode_block(buffer);

    const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{data.data(), data.size()}};
    REQUIRE(block.type() == osmium::item_type::way);
    // Unused empty string, "foo", "highway", "primary", "bar", "residential"
    // and the empty user name of way 7.
    REQUIRE(block.stringtable_size() == 7);

    const auto objects = encoded_objects(block);
    REQUIRE(objects.size() == 3);
    REQUIRE(objects[0].id == -3);
    REQUIRE(objects[0].version == 2);
    REQUIRE(objects[0].timestamp == osmium::Timestamp{"2020-01-01T00:00:00Z"});
    REQUIRE(objects[0].visible);
    REQUIRE(objects[1].id == 7);
    REQUIRE(objects[1].version == 5);
    REQUIRE_FALSE(objects[1].timestamp.valid());
    REQUIRE(objects[2].id == 8);

    REQUIRE(string_indexes(block, objects[0]).size() == 3);
    REQUIRE(string_indexes(block, objects[1]).size() == 1);
    REQUIRE(string_indexes(block, objects[2]).size() == 3);
}

TEST_CASE("Encoded objects are not available for some PBF blocks") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_node(buffer, _id(1), _version(1), _location(1.0, 2.0));

    SECTION("dense nodes") {
        const auto data = encode_block(buffer);
        const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{data.data(), data.size()}};
        REQUIRE(block.type() == osmium::item_type::undefined);
    }

    SECTION("nodes") {
        const auto data = encode_block(buffer, "pbf,pbf_compression=none,pbf_dense_nodes=false");
        const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{data.data(), data.size()}};
        REQUIRE(block.type() == osmium::item_type::undefined);
    }

    SECTION("empty block") {
        const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{}};
        REQUIRE(block.type() == osmium::item_type::undefined);
    }

    SECTION("other date granularity") {
        osmium::memory::Buffer ways{1024UL, osmium::memory::Buffer::auto_grow::yes};
        osmium::builder::add_way(ways, _id(1), _version(1), _nodes({1, 2}));
        auto data = encode_block(ways);
        data += std::string{"\x90\x01\x01", 3}; // date_granularity = 1
        const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{data.data(), data.size()}};
        REQUIRE(block.type() == osmium::item_type::undefined);
    }
}

TEST_CASE("Copy encoded objects into a PBF block continuing the string table") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer input{1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_relation(input, _id(1), _version(1), _user("foo"), _member(osmium::item_type::way, 1, "outer"), _tag("type", "multipolygon"));
    osmium::builder::add_relation(input, _id(2), _version(1), _user("bar"), _member(osmium::item_type::way, 2, "inner"), _tag("type", "route"));
    osmium::builder::add_relation(input, _id(4), _version(3), _user("foo"), _member(osmium::item_type::node, 7, ""), _tag("name", "x"));
    const auto data = encode_block(input);
    const osmium::io::detail::PBFEncodedPrimitiveBlock block{osmium::io::detail::data_view{data.data(), data.size()}};
    REQUIRE(block.type() == osmium::item_type::relation);
    const auto objects = encoded_objects(block);
    REQUIRE(objects.size() == 3);

    // Keep relations 1 and 4, replace 2, add 3.
    osmium::memory::Buffer changes{1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_relation(changes, _id(2), _version(2), _user("bar"), _member(osmium::item_type::way, 2, "outer"), _tag("type", "boundary"));
    osmium::builder::add_relation(changes, _id(3), _version(1), _user("baz"), _member(osmium::item_type::relation, 1, "subarea"), _tag("type", "route"));
    auto change = changes.cbegin<osmium::Relation>();

    const osmium::io::File file{"test.osm.pbf", "pbf,pbf_compression=none"};
    const auto options = osmium::io::detail::make_pbf_output_options(file);
    osmium::io::detail::PrimitiveBlock primitive_block{options, osmium::io::detail::OSMFormat::PrimitiveGroup::repeated_Relation_relations, block.stringtable()};
    primitive_block.add_encoded_object(objects[0].data);
    primitive_block.add_relation(*change++);
    primitive_block.add_relation(*change);
    primitive_block.add_encoded_object(objects[2].data);
    REQUIRE(primitive_block.count() == 4);
    const auto spliced = osmium::io::detail::serialize_primitive_block(primitive_block);

    // Only "baz", "boundary", and "subarea" are new strings.
    const osmium::io::detail::PBFEncodedPrimitiveBlock spliced_block{osmium::io::detail::data_view{spliced.data(), spliced.size()}};
    REQUIRE(spliced_block.type() == osmium::item_type::relation);
    REQUIRE(spliced_block.stringtable_size() == block.stringtable_size() + 3);

    const auto result = decode_block(spliced);
    std::vector<std::string> opl;
    for (const auto& relation : result.select<osmium::Relation>()) {
        std::string s{std::to_string(relation.id()) + " v" + std::to_string(relation.version()) + " " + relation.user()};
        for (const auto& member : relation.members()) {
            s += " " + std::string(1, osmium::item_type_to_char(member.type())) + std::to_string(member.ref()) + "@" + member.role();
        }
        for (const auto& tag : relation.tags()) {
            s += " " + std::string{tag.key()} + "=" + tag.value();
        }
        opl.push_back(s);
    }
    const std::vector<std::string> expected{
        "1 v1 foo w1@outer type=multipolygon",
        "2 v2 bar w2@outer type=boundary",
        "3 v1 baz r1@subarea type=route",
        "4 v3 foo n7@ name=x"};
    REQUIRE(opl == expected);
}

TEST_CASE("Writing PBF blob to non-PBF file throws") {
    osmium::io::Writer writer{"test-pbf-blobs-out.opl", osmium::io::overwrite::allow};
    REQUIRE_THROWS_AS(writer.write_pbf_blob(std::string{"foo"}), osmium::unsupported_file_format_error);
}

namespace {

std::string write_single_blob(const char* filename, const char* format, osmium::memory::Buffer&& buffer) {
    osmium::io::Writer writer{osmium::io::File{filename, format}, osmium::io::overwrite::allow};
    writer(std::move(buffer));
    writer.close();

    auto blobs = read_data_blobs(filename);
    REQUIRE(blobs.size() == 1);
    return std::move(blobs.front());
}

void add_test_node(osmium::memory::Buffer& buffer, osmium::object_id_type id, uint32_t version, const char* user) {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)
    const auto n = static_cast<uint32_t>(id);
    if (id % 3 == 0) {
        osmium::builder::add_node(buffer, _id(id), _version(version), _timestamp(1600000000U + n), _cid(n * 1000), _uid(n % 7),
                                  _user(user), _location(id * 0.01, -id * 0.01),
                                  _tag("name", "node" + std::to_string(id)), _tag("amenity", ""));
    } else {
        osmium::builder::add_node(buffer, _id(id), _version(version), _timestamp(1600000000U + n), _cid(n * 1000), _uid(n % 7),
                                  _user(user), _location(id * 0.01, -id * 0.01));
    }
}

std::vector<std::string> describe_nodes(const osmium::memory::Buffer& buffer) {
    std::vector<std::string> nodes;
    for (const auto& node : buffer.select<osmium::Node>()) {
        std::string str = std::to_string(node.id()) + " v" + std::to_string(node.version()) + " t" + node.timestamp().to_iso() +
                          " c" + std::to_string(node.changeset()) + " i" + std::to_string(node.uid()) + " u" + node.user() +
                          " x" + std::to_string(node.location().x()) + " y" + std::to_string(node.location().y()) + " T";
        for (const auto& tag : node.tags()) {
            str += std::string{tag.key()} + "=" + tag.value() + ",";
        }
        nodes.push_back(str);
    }
    return nodes;
}

std::vector<std::string> decode_nodes(const std::string& primitive_block) {
    osmium::io::detail::PBFPrimitiveBlockDecoder decoder{osmium::io::detail::data_view{primitive_block.data(), primitive_block.size()},
                                                         osmium::osm_entity_bits::object,
                                                         osmium::io::read_meta::yes};
    osmium::memory::Buffer buffer = decoder();
    std::vector<osmium::memory::Buffer> buffers;
    while (buffer.has_nested_buffers()) {
        buffers.push_back(std::move(*buffer.get_last_nested()));
    }
    buffers.push_back(std::move(buffer));

    std::vector<std::string> nodes;
    for (const auto& b : buffers) {
        const auto n = describe_nodes(b);
        nodes.insert(nodes.end(), n.begin(), n.end());
    }
    return nodes;
}

} // anonymous namespace

TEST_CASE("Copy all nodes of a PBF block with DenseNodesBlockEditor") {
    osmium::memory::Buffer buffer{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 100; ++id) {
        add_test_node(buffer, id * 10, static_cast<uint32_t>(id), id % 2 ? "foo" : "bar");
    }
    const auto blob = write_single_blob("test-pbf-dense-editor.osm.pbf", "pbf", std::move(buffer));

    std::string uncompressed;
    const auto options = osmium::io::detail::make_pbf_output_options(osmium::io::File{"test.osm.pbf"});
    osmium::io::detail::DenseNodesBlockEditor editor{osmium::io::detail::decode_blob(blob, uncompressed), options};
    REQUIRE(editor.valid());

    const auto ids = editor.node_ids();
    REQUIRE(ids.size() == 100);
    REQUIRE(ids.front() == 10);
    REQUIRE(ids.back() == 1000);

    osmium::object_id_type id = 1;
    while (editor.next()) {
        REQUIRE(editor.node_id() == id * 10);
        REQUIRE(editor.node_version() == id);
        REQUIRE(editor.node_timestamp() == osmium::Timestamp{1600000000U + static_cast<uint32_t>(id * 10)});
        editor.copy_node();
        ++id;
    }
    REQUIRE(editor.valid());
    REQUIRE(id == 101);
    REQUIRE(editor.count() == 100);
    REQUIRE(editor.unused_strings() == 0);

    // The nodes are copied as they are, so the block is the same.
    REQUIRE(osmium::io::detail::serialize_blob(editor.serialize(), options.use_compression, options.compression_level) == blob);
}

TEST_CASE("Drop and add nodes of a PBF block with DenseNodesBlockEditor") {
    osmium::memory::Buffer buffer{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    for (osmium::object_id_type id = 1; id <= 100; ++id) {
        add_test_node(buffer, id * 10, 1, "foo");
    }
    const auto blob = write_single_blob("test-pbf-dense-editor.osm.pbf", "pbf", std::move(buffer));

    // New nodes with new and existing strings
    osmium::memory::Buffer changes{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    add_test_node(changes, 5, 1, "new");
    add_test_node(changes, 20, 2, "foo");
    add_test_node(changes, 1001, 1, "");
    auto change = changes.select<osmium::Node>().begin();

    // The expected result: node 5 inserted before 10, 20 replaced, 30
    // dropped, 1001 added at the end.
    osmium::memory::Buffer expected{1024UL * 1024UL, osmium::memory::Buffer::auto_grow::yes};
    add_test_node(expected, 5, 1, "new");
    add_test_node(expected, 10, 1, "foo");
    add_test_node(expected, 20, 2, "foo");
    for (osmium::object_id_type id = 4; id <= 100; ++id) {
        add_test_node(expected, id * 10, 1, "foo");
    }
    add_test_node(expected, 1001, 1, "");

    std::string uncompressed;
    const auto options = osmium::io::detail::make_pbf_output_options(osmium::io::File{"test.osm.pbf"});
    osmium::io::detail::DenseNodesBlockEditor editor{osmium::io::detail::decode_blob(blob, uncompressed), options};
    REQUIRE(editor.valid());

    while (editor.next()) {
        switch (editor.node_id()) {
            case 10:
                editor.add_node(*change++);
                editor.copy_node();
                break;
            case 20:
                editor.add_node(*change++);
                editor.drop_node();
                break;
            case 30:
                editor.drop_node();
                break;
            default:
                editor.copy_node();
        }
    }
    editor.add_node(*change++);
    REQUIRE(editor.valid());
    REQUIRE(editor.count() == 101);

    // The name of node 30 is not used any more.
    REQUIRE(editor.unused_strings() == 1);

    REQUIRE(decode_nodes(editor.serialize()) == describe_nodes(expected));
}

TEST_CASE("DenseNodesBlockEditor does not support all blocks") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL, osmium::memory::Buffer::auto_grow::yes};
    std::string format{"pbf"};

    SECTION("ways") {
        osmium::builder::add_way(buffer, _id(1), _version(1), _nodes({1, 2}));
    }
    SECTION("nodes without dense nodes") {
        format = "pbf,pbf_dense_nodes=false";
        osmium::builder::add_node(buffer, _id(1), _version(1), _location(1.0, 2.0));
    }
    SECTION("nodes without some metadata") {
        format = "pbf,add_metadata=version+timestamp";
        osmium::builder::add_node(buffer, _id(1), _version(1), _location(1.0, 2.0));
    }
    SECTION("deleted node") {
        format = "osh.pbf";
        osmium::builder::add_node(buffer, _id(1), _version(1), _location(1.0, 2.0));
        osmium::builder::add_node(buffer, _id(2), _version(2), _deleted());
    }

    const auto blob = write_single_blob("test-pbf-dense-editor-fail.osm.pbf", format.c_str(), std::move(buffer));
    std::string uncompressed;
    const auto options = osmium::io::detail::make_pbf_output_options(osmium::io::File{"test.osm.pbf"});
    const osmium::io::detail::DenseNodesBlockEditor editor{osmium::io::detail::decode_blob(blob, uncompressed), options};
    REQUIRE_FALSE(editor.valid());
}

TEST_CASE("DenseNodesBlockEditor needs output options for dense nodes with all metadata") {
    using namespace osmium::builder::attr; // NOLINT(google-build-using-namespace)

    osmium::memory::Buffer buffer{1024UL, osmium::memory::Buffer::auto_grow::yes};
    osmium::builder::add_node(buffer, _id(1), _version(1), _location(1.0, 2.0));
    const auto blob = write_single_blob("test-pbf-dense-editor-options.osm.pbf", "pbf", std::move(buffer));

    std::string format;
    SECTION("no dense nodes") {
        format = "pbf,pbf_dense_nodes=false";
    }
    SECTION("not all metadata") {
        format = "pbf,add_metadata=version";
    }
    SECTION("history") {
        format = "osh.pbf";
    }

    std::string uncompressed;
    const auto options = osmium::io::detail::make_pbf_output_options(osmium::io::File{"test.osm.pbf", format});
    const osmium::io::detail::DenseNodesBlockEditor editor{osmium::io::detail::decode_blob(blob, uncompressed), options};
    REQUIRE_FALSE(editor.valid());
}
