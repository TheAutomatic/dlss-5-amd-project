// No GPU: exercise the shipped PAL inputs, corruption gates and compiler isolation.
#include <nr_pipeline_binary.hpp>
#include <filesystem>
#include <thread>
#include <iostream>

static void require(bool value,const char* why) { if(!value)throw std::runtime_error(why); }
template<class F> static void rejects(F&& f) {
    bool rejected=false;
    try { f(); } catch(const std::exception&) { rejected=true; }
    require(rejected,"malformed binary was accepted");
}
int main(int argc,char** argv) try {
    require(argc==3,"record directory and scratch cache path required");
    unsigned count=0;
    for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
        if(entry.path().extension()!=".nrp")continue;
        const auto bytes=nr::pal::load(entry.path().string());
        const auto record=nr::pal::Record::parse(bytes);
        require(entry.path().filename()==nr::binary::keyname(record.spv_hash),"record filename mismatch");
        require(!nr::pal::prologue(record).empty(),"empty PAL argument adapter");
        for(size_t end:{size_t(0),size_t(95),bytes.size()-1})
            rejects([&]{nr::pal::Record::parse(nr::pal::Bytes(bytes.begin(),bytes.begin()+end));});
        auto bad=bytes;bad.back()^=1;
        rejects([&]{nr::pal::Record::parse(bad);});
        bad=bytes;bad[76]=1;
        rejects([&]{nr::pal::Record::parse(bad);});
        bad=bytes;bad[68]=33;
        rejects([&]{nr::pal::Record::parse(bad);});
        bad=bytes;bad[48]=129;
        rejects([&]{nr::pal::Record::parse(bad);});
        ++count;
    }
    require(count==58,"unexpected pinned record inventory");
    rejects([]{nr::pal::unpack({0x82,0xa1,'x',0x01,0xa1,'x',0x02});});
    rejects([]{nr::pal::unpack({0xdb,0xff,0xff,0xff,0xff});});

    VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    key.keySize=2;key.key[0]=42;key.key[1]=17;
    nr::pal::Bytes blob{1,2,3,4},loaded;
    nr::binary::save_cached(argv[2],key,blob);
    VkPipelineBinaryKeyKHR read{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    require(nr::binary::load_cached(argv[2],read,loaded) && read.keySize==2 &&
            read.key[0]==42 && read.key[1]==17 && loaded==blob,"cache round trip failed");
    auto corrupt=nr::pal::load(argv[2]);corrupt.back()^=1;
    {std::ofstream out(argv[2],std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<char*>(corrupt.data()),corrupt.size());}
    require(!nr::binary::load_cached(argv[2],read,loaded),"corrupt cache accepted");
    {std::ofstream out(argv[2],std::ios::binary|std::ios::trunc);out.write("NRAC",4);}
    require(!nr::binary::load_cached(argv[2],read,loaded),"truncated cache accepted");
    std::filesystem::remove(argv[2]);

    nr::binary::configure("main",{},"",{});
    nr::binary::mode().imported=7;
    bool isolated=false;
    std::thread other([&]{
        isolated=nr::binary::directory().empty();
        nr::binary::configure("other",{},"",{});
        isolated &= nr::binary::directory()=="other" && nr::binary::mode().imported==0;
    });other.join();
    require(isolated && nr::binary::directory()=="main" && nr::binary::mode().imported==7,"compiler state leaked between builders");
    nr::binary::configure("",{},"",{});
    require(nr::binary::directory().empty() && !nr::binary::mode().imported,"compiler mode did not reset");
    auto d1=reinterpret_cast<VkDevice>(uintptr_t(1)),d2=reinterpret_cast<VkDevice>(uintptr_t(2));
    nr::binary::mark(d1,101);nr::binary::mark(d2,202);
    require(nr::binary::device_identity(d1)==101 && nr::binary::device_identity(d2)==202,"device cache identities mixed");
    nr::binary::forget(d1);nr::binary::forget(d2);
    require(!nr::binary::enabled(d1),"destroyed device remained enabled");
    try { nr::binary::check(VK_ERROR_DEVICE_LOST,"test"); }
    catch(const nr::binary::VulkanError& e) {require(std::string(e.what()).find("VkResult=-4")!=std::string::npos,"lost-device classification missing");}
    std::cout<<"MOCHIZUKI_ACO_CPU_OK records="<<count<<"\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
