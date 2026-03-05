# cpp-thread-pool

基于c++实现的线程池

## plan

- [x] 实现基础结构
- [ ] 添加单例模式
- [ ] 优先级机制
- [ ] Work-Stealing 机制

## debug

关于参数包展开（c++11）和折叠表达式（c++17）

`std::forward<Args>(args)...` 和 `(std::forward<Args>(args), ...)` 完全不一样。

例如传入 `args = {1, 'c', "s"}`

参数包展开用于初始化列表，只有**语法环境允许多个元素时才合法**，如：

```c++
{ args... }      // 初始化列表
bind(f, args...) // 接受变参模板的函数初始化
tuple<args...>   // 接受变参模板的容器初始化
```

而折叠表达式用来把运算符和参数链接在一起;

```c++
((f(args)), ...)
```

编译后为

```c++
(f(arg1)), (f(arg2)), (f(arg3))
```

例如

```c++
#include <iostream>
#include <string>

template<class ... Args>
void print(Args&& ... args){
    ((std::cout << (args + 'c') << std::endl), ...);
}

int main() {
    print(1, 3.5, 0x3f3f3f3f, 'c', (std::string)"str");
    return 0;
}
```

输出

```text
100
102.5
1061109666
198
strc
```