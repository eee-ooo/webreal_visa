#ifndef WRVISA_RESOURCE_FIND_EXPRESSION_H
#define WRVISA_RESOURCE_FIND_EXPRESSION_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace wrvisa {

struct FindNode;

class FindExpression {
public:
    static std::optional<FindExpression> compile(std::string_view expression,
                                                 std::string& error);
    bool matches(std::string_view value) const;

private:
    explicit FindExpression(std::shared_ptr<const FindNode> root)
        : root_(std::move(root)) {}

    std::shared_ptr<const FindNode> root_;
};

}  // namespace wrvisa

#endif
