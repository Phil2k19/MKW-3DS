#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
namespace mkw {
// Original ItemSlot.bin: count, then [rank count, item count, item-major bytes].
// Copy the selected 12-racer tables so no race-heap/archive pointer escapes reset.
class ItemOdds {
public:
    bool load(const void* bytes,std::size_t size,unsigned localTable,unsigned cpuTable){
        tables_={};ready_=false;
        if(!bytes||size<1)return false;
        const auto* data=static_cast<const uint8_t*>(bytes);std::size_t offset=1;unsigned found=0;
        for(unsigned table=0;table<data[0];++table){
            if(size-offset<2)return false;
            const unsigned ranks=data[offset++],items=data[offset++];
            if(!ranks||!items||std::size_t(ranks)*items>size-offset)return false;
            for(unsigned role=0;role<2;++role)if(table==(role?cpuTable:localTable)){
                if(ranks!=12||items!=19)return false;
                for(unsigned item=0;item<19;++item)for(unsigned rank=0;rank<12;++rank)
                    tables_[role][rank][item]=data[offset+item*ranks+rank];
                ++found;
            }
            offset+=std::size_t(ranks)*items;
        }
        ready_=found==2&&offset==size;return ready_;
    }
    bool ready() const{return ready_;}
    const std::array<uint8_t,19>& weights(bool cpu,unsigned rank)const{return tables_[cpu?1:0][std::clamp(rank,1u,12u)-1];}
private:
    std::array<std::array<std::array<uint8_t,19>,12>,2> tables_{};
    bool ready_=false;
};
// The course-object drop table, ItemSlot.bin table 5 (16 rows): decideItem (0x807BB42C)
// reads row setting1 - 1 of it for a broken crate (Breakable 0x8076EDE4). The shipped crates
// use row 5: Banana 100, Mushroom 100.
class CourseDropOdds {
public:
    bool load(const void* bytes,std::size_t size){
        rows_={};ready_=false;
        if(!bytes||size<1)return false;
        const auto* data=static_cast<const uint8_t*>(bytes);std::size_t offset=1;bool found=false;
        for(unsigned table=0;table<data[0];++table){
            if(size-offset<2)return false;
            const unsigned rows=data[offset++],items=data[offset++];
            if(!rows||!items||std::size_t(rows)*items>size-offset)return false;
            if(table==5){
                if(rows!=16||items!=19)return false;
                for(unsigned item=0;item<19;++item)for(unsigned row=0;row<16;++row)
                    rows_[row][item]=data[offset+item*rows+row];
                found=true;
            }
            offset+=std::size_t(rows)*items;
        }
        ready_=found&&offset==size;return ready_;
    }
    bool ready() const{return ready_;}
    // Row `slot` - 1; nullptr for a slot outside 1..16.
    const std::array<uint8_t,19>* weights(unsigned slot)const{return ready_&&slot>=1&&slot<=16?&rows_[slot-1]:nullptr;}
private:
    std::array<std::array<uint8_t,19>,16> rows_{};
    bool ready_=false;
};
}
