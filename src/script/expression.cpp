#include "script/expression.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace artc {
namespace {
int32_t Wrap(uint64_t n) {
    const uint32_t bits=static_cast<uint32_t>(n);
    return bits<=INT32_MAX ? static_cast<int32_t>(bits) :
        static_cast<int32_t>(int64_t(bits)-0x100000000LL);
}
struct Value {
    std::string text;
    int32_t number=0;
    bool numeric=true;
    Value(int64_t n=0) : number(Wrap(n)) {}
    explicit Value(std::string s, bool literal=false) : text(std::move(s)),numeric(false) {
        if(literal || text.empty()) return;
        char* end=nullptr;
        const auto n=std::strtoll(text.c_str(),&end,0);
        if(end==text.c_str()+text.size()) { number=Wrap(n);numeric=true; }
    }
    std::string String() const { return numeric ? std::to_string(number) : text; }
    bool True() const { return numeric ? number!=0 : !text.empty(); }
};
class Parser {
public:
    Parser(const std::string& source,const std::function<std::string(const std::string&)>& variable)
        : source_(source),variable_(variable) {}
    bool Run(std::string& out) {
        const auto value=Expression(1,true);
        Space();
        if(!ok_ || pos_!=source_.size()) return false;
        out=value.String();return true;
    }
private:
    void Space() { while(pos_<source_.size() && std::isspace(static_cast<unsigned char>(source_[pos_]))) ++pos_; }
    bool Take(char c) { Space();if(pos_<source_.size() && source_[pos_]==c) {++pos_;return true;} return false; }
    Value Atom(bool evaluate) {
        if(++depth_>128) {ok_=false;--depth_;return {};}
        Space();Value v;
        if(Take('!')) v=Value(!Atom(evaluate).True());
        else if(Take('~')) v=Value(~Atom(evaluate).number);
        else if(Take('-')) v=Value(-int64_t(Atom(evaluate).number));
        else if(Take('+')) v=Atom(evaluate);
        else if(Take('(')) {v=Expression(1,evaluate);if(!Take(')')) ok_=false;}
        else if(pos_<source_.size() && (source_[pos_]=='\'' || source_[pos_]=='"')) {
            const char quote=source_[pos_++];std::string text;
            while(pos_<source_.size() && source_[pos_]!=quote) {
                char c=source_[pos_++];
                if(c=='\\' && pos_<source_.size()) {
                    c=source_[pos_++];
                    if(c=='n') c='\n';else if(c=='r') c='\r';else if(c=='t') c='\t';
                }
                text+=c;
            }
            if(pos_==source_.size()) ok_=false;else ++pos_;
            v=Value(std::move(text),true);
        } else {
            Take('$');Space();const size_t start=pos_;
            if(pos_<source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_]))) {
                uint64_t n=0;int radix=10;
                if(source_.compare(pos_,2,"0x")==0 || source_.compare(pos_,2,"0X")==0) {radix=16;pos_+=2;}
                const size_t digits=pos_;
                while(pos_<source_.size()) {
                    const char c=source_[pos_];
                    const int d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : radix;
                    if(d>=radix) break;
                    n=static_cast<uint32_t>(n*radix+d);++pos_;
                }
                if(pos_==digits) ok_=false;
                v=Value(Wrap(n));
            } else {
                while(pos_<source_.size()) {
                    const unsigned char c=source_[pos_];
                    if(!std::isalnum(c) && c!='_' && c!='.' && c<128) break;
                    ++pos_;
                }
                if(start==pos_) ok_=false;
                const std::string name=source_.substr(start,pos_-start);
                if(name=="true") v=Value(1);
                else if(name=="false") v=Value(0);
                else if(evaluate) v=Value(variable_(name));
            }
        }
        --depth_;return v;
    }
    std::pair<std::string,int> Op() {
        Space();
        static const std::pair<const char*,int> ops[]={{"||",1},{"&&",2},{"|",3},{"^",4},{"&",5},
            {"==",6},{"!=",6},{"<=",7},{">=",7},{"<<",8},{">>",8},{"<",7},{">",7},
            {"+",9},{"-",9},{"*",10},{"/",10},{"%",10}};
        for(auto op:ops) if(source_.compare(pos_,std::char_traits<char>::length(op.first),op.first)==0) return {op.first,op.second};
        return {{},0};
    }
    Value Expression(int minimum,bool evaluate) {
        Value left=Atom(evaluate);
        while(ok_) {
            const auto op=Op();if(op.second<minimum) break;
            pos_+=op.first.size();
            const bool rhs=evaluate && !(op.first=="&&" && !left.True()) && !(op.first=="||" && left.True());
            const Value right=Expression(op.second+1,rhs);
            if(evaluate) left=Apply(op.first,left,right,rhs);
        }
        return left;
    }
    Value Apply(const std::string& op,const Value& a,const Value& b,bool rhs) {
        if(op=="&&") return Value(a.True() && rhs && b.True());
        if(op=="||") return Value(a.True() || (rhs && b.True()));
        const int cmp=a.numeric && b.numeric ? (a.number>b.number)-(a.number<b.number) : a.String().compare(b.String());
        if(op=="==") return Value(cmp==0);if(op=="!=") return Value(cmp!=0);
        if(op=="<=") return Value(cmp<=0);if(op==">=") return Value(cmp>=0);
        if(op=="<") return Value(cmp<0);if(op==">") return Value(cmp>0);
        if(op=="+" && (!a.numeric || !b.numeric)) return Value(a.String()+b.String(),true);
        if(!a.numeric || !b.numeric) {ok_=false;return {};}
        const int64_t x=a.number,y=b.number;
        if(op=="+") return Value(x+y);if(op=="-") return Value(x-y);if(op=="*") return Value(x*y);
        if(op=="/" || op=="%") {
            if(y==0) {ok_=false;return {};}
            return Value(op=="/" ? x/y : x%y);
        }
        if(op=="&") return Value(x&y);if(op=="|") return Value(x|y);if(op=="^") return Value(x^y);
        if(y<0 || y>31) {ok_=false;return {};}
        if(op=="<<") return Value(Wrap(uint64_t(uint32_t(x))<<y));
        return Value(x>>y);
    }
    const std::string& source_;
    const std::function<std::string(const std::string&)>& variable_;
    size_t pos_=0;int depth_=0;bool ok_=true;
};
}
bool EvaluateExpression(const std::string& expression,
                        const std::function<std::string(const std::string&)>& variable,
                        std::string& result) {
    return Parser(expression,variable).Run(result);
}
} // namespace artc
