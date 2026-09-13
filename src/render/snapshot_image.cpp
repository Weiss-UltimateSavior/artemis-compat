#include "render/snapshot_image.h"
#include <zlib.h>
#include <algorithm>
#include <cmath>
namespace artc {
namespace {
void BigEndian(std::vector<uint8_t>& out,uint32_t value) {
    for(int shift=24;shift>=0;shift-=8)out.push_back(uint8_t(value>>shift));
}
void Chunk(std::vector<uint8_t>& out,const char* name,const std::vector<uint8_t>& bytes) {
    BigEndian(out,uint32_t(bytes.size()));const auto start=out.size();
    out.insert(out.end(),name,name+4);out.insert(out.end(),bytes.begin(),bytes.end());
    BigEndian(out,crc32(0,out.data()+start,uInt(bytes.size()+4)));
}
bool Size(int w,int h) {return w>0 && h>0 && w<=8192 && h<=8192 && uint64_t(w)*h<=16777216;}
}
bool SnapshotImage::EncodePng(int w,int h,std::vector<uint8_t>& output) const {
    if(!Size(width,height) || !Size(w,h) || rgba.size()!=size_t(width)*height*4)return false;
    const size_t stride=size_t(w)*4+1;std::vector<uint8_t> pixels(stride*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const double sx=std::clamp((x+0.5)*width/w-0.5,0.0,double(width-1));
        const double sy=std::clamp((y+0.5)*height/h-0.5,0.0,double(height-1));
        const int x0=int(sx),y0=int(sy),x1=std::min(x0+1,width-1),y1=std::min(y0+1,height-1);
        const double fx=sx-x0,fy=sy-y0;
        const size_t at[]={size_t(y0*width+x0)*4,size_t(y0*width+x1)*4,size_t(y1*width+x0)*4,size_t(y1*width+x1)*4};
        const double weights[]={(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
        double color[4]={};
        for(int i=0;i<4;++i) {
            const double alpha=rgba[at[i]+3]/255.0;const double weight=weights[i];
            for(int c=0;c<3;++c)color[c]+=rgba[at[i]+c]*alpha*weight;
            color[3]+=alpha*weight;
        }
        const size_t dst=size_t(y)*stride+1+x*4;
        for(int c=0;c<3;++c)pixels[dst+c]=uint8_t(std::lround(color[3]>0?color[c]/color[3]:0));
        pixels[dst+3]=uint8_t(std::lround(color[3]*255));
    }
    uLongf length=compressBound(pixels.size());std::vector<uint8_t> compressed(length);
    if(compress2(compressed.data(),&length,pixels.data(),pixels.size(),Z_BEST_SPEED)!=Z_OK)return false;
    compressed.resize(length);
    std::vector<uint8_t> png={137,80,78,71,13,10,26,10},header;
    BigEndian(header,w);BigEndian(header,h);header.insert(header.end(),{8,6,0,0,0});
    Chunk(png,"IHDR",header);Chunk(png,"IDAT",compressed);Chunk(png,"IEND",{});
    output=std::move(png);return true;
}
}
