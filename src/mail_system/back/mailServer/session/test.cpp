#include <iostream>
using namespace std;
#define forr(i, _begin, _end) for(int i = _begin; i < _end; i++)
int main(){
    try{
        // string s = "Hello, World!\n\nThis is a test.";
        // auto pos = s.find("\n\n");
        // cout << s[pos + 2];
        string t = "123\r\n.";
        auto pos = t.find("2");
        cout << t.substr(pos) << endl;
        // auto pos = t.find("\n\n");
        // cout << t.substr(0, pos) << endl << "body:" << t.substr(pos + 2) << endl;
    } catch (const std::exception& e) {
        cout << e.what() << endl;
    }
    return 0;
}