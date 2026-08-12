#ifndef WRVISA_RESOURCE_FIND_EXPRESSION_H
#define WRVISA_RESOURCE_FIND_EXPRESSION_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "resource/resource_parser.h"

namespace wrvisa {

struct FindNode;
struct FindAttributeNode;

class FindExpression {
public:
    static std::optional<FindExpression> compile(std::string_view expression,
                                                 std::string& error);
    bool matches(std::string_view value) const;
    bool matches(const ResourceDescriptor& resource) const;

private:
    FindExpression(std::shared_ptr<const FindNode> root,
                   std::shared_ptr<const FindAttributeNode> attributes)
        : root_(std::move(root)), attributes_(std::move(attributes)) {}

    std::shared_ptr<const FindNode> root_;
    std::shared_ptr<const FindAttributeNode> attributes_;
};

}  // namespace wrvisa

#endif
