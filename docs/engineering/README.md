# 本 fork 的工程规范

以下五份文件来自用户的 `EngineeringStandards_20261105/rules`，保留原文：

- [CODE_FORMAT.md](CODE_FORMAT.md)：按语言执行格式，保留现有一致风格和公开接口。
- [METHOD_COMMENTS.md](METHOD_COMMENTS.md)：所有具名方法的用途与关键步骤说明。
- [READABLE_CODE.md](READABLE_CODE.md)：命名、单位、职责和错误语义。
- [CPP_PYTHON_RULES.md](CPP_PYTHON_RULES.md)：独立 C++ 算法与现有语言的绑定边界。
- [AI_CODE_GENERATION.md](AI_CODE_GENERATION.md)：阅读调用链、最小修改和真实验证。

这里是项目内的规范入口，不安装用户全局规则，不参与程序运行。原文中测量项目、C#、
Windows 等限定条件只适用于其原项目。旧工程 Python、JS 和原生 Rust 延续所属框架及模块风格；
新增传统算法保持 C++17，C ABI 不因命名规则改变。

新建自有代码默认四空格；CSS、JSON 等配置与标记文件使用两空格。移植旧实现时仅整理涉及的
自有模块，不重排整个上游。Rust 补丁保留原生仓库的约定和 `cargo fmt` 入口。
