#include "resource/find_expression.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace wrvisa {

struct FindNode {
    enum class Type { empty, literal, any, character_class, concatenation, alternate, repeat };

    Type type{Type::empty};
    char literal{0};
    bool negated{false};
    bool one_or_more{false};
    std::vector<std::pair<char, char>> ranges;
    std::vector<std::shared_ptr<const FindNode>> children;
};

struct FindAttributeNode {
    enum class Type { relation, logical_and, logical_or, logical_not };
    enum class Attribute { intf_type, intf_num, rsrc_class, rsrc_name, asrl_baud };
    enum class Compare { equal, not_equal, less, less_equal, greater, greater_equal };

    Type type{Type::relation};
    Attribute attribute{Attribute::intf_type};
    Compare compare{Compare::equal};
    bool string_value{false};
    std::int64_t number{0};
    std::string text;
    std::vector<std::shared_ptr<const FindAttributeNode>> children;
};

namespace {

char fold(char value) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(value)));
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::shared_ptr<const FindNode> parse(std::string& error) {
        if (input_.empty() || input_.size() >= 256 || input_.find('{') != input_.npos ||
            input_.find('}') != input_.npos) {
            error = "empty, oversized, or attribute-filter expression";
            return {};
        }
        auto root = parse_alternate(error, 0);
        if (!root || position_ != input_.size()) {
            if (error.empty()) {
                error = "unexpected token";
            }
            return {};
        }
        return root;
    }

private:
    std::shared_ptr<const FindNode> make(FindNode node, std::string& error) {
        if (++nodes_ > 512) {
            error = "expression is too complex";
            return {};
        }
        return std::make_shared<FindNode>(std::move(node));
    }

    std::shared_ptr<const FindNode> parse_alternate(std::string& error,
                                                    std::size_t depth) {
        if (depth > 32) {
            error = "expression nesting is too deep";
            return {};
        }
        std::vector<std::shared_ptr<const FindNode>> branches;
        auto branch = parse_concatenation(error, depth);
        if (!branch) {
            return {};
        }
        branches.push_back(std::move(branch));
        while (position_ < input_.size() && input_[position_] == '|') {
            ++position_;
            auto next = parse_concatenation(error, depth);
            if (!next) {
                return {};
            }
            branches.push_back(std::move(next));
        }
        if (branches.size() == 1) {
            return branches.front();
        }
        FindNode node;
        node.type = FindNode::Type::alternate;
        node.children = std::move(branches);
        return make(std::move(node), error);
    }

    std::shared_ptr<const FindNode> parse_concatenation(std::string& error,
                                                        std::size_t depth) {
        std::vector<std::shared_ptr<const FindNode>> sequence;
        while (position_ < input_.size() && input_[position_] != ')' &&
               input_[position_] != '|') {
            auto atom = parse_atom(error, depth);
            if (!atom) {
                return {};
            }
            if (position_ < input_.size() &&
                (input_[position_] == '*' || input_[position_] == '+')) {
                FindNode repeat;
                repeat.type = FindNode::Type::repeat;
                repeat.one_or_more = input_[position_] == '+';
                repeat.children.push_back(std::move(atom));
                ++position_;
                atom = make(std::move(repeat), error);
                if (!atom) {
                    return {};
                }
                if (position_ < input_.size() &&
                    (input_[position_] == '*' || input_[position_] == '+')) {
                    error = "repeated quantifier";
                    return {};
                }
            }
            sequence.push_back(std::move(atom));
        }
        if (sequence.empty()) {
            error = "empty branch";
            return {};
        }
        if (sequence.size() == 1) {
            return sequence.front();
        }
        FindNode node;
        node.type = FindNode::Type::concatenation;
        node.children = std::move(sequence);
        return make(std::move(node), error);
    }

    std::shared_ptr<const FindNode> parse_atom(std::string& error, std::size_t depth) {
        if (position_ >= input_.size()) {
            error = "missing atom";
            return {};
        }
        const char token = input_[position_++];
        if (token == '(') {
            auto group = parse_alternate(error, depth + 1u);
            if (!group || position_ >= input_.size() || input_[position_] != ')') {
                if (error.empty()) {
                    error = "unclosed group";
                }
                return {};
            }
            ++position_;
            return group;
        }
        if (token == '[') {
            return parse_class(error);
        }
        if (token == '?' ) {
            FindNode node;
            node.type = FindNode::Type::any;
            return make(std::move(node), error);
        }
        if (token == '*' || token == '+' || token == ')') {
            error = "operator without atom";
            return {};
        }
        char literal = token;
        if (token == '\\') {
            if (position_ >= input_.size()) {
                error = "dangling escape";
                return {};
            }
            literal = input_[position_++];
        }
        FindNode node;
        node.type = FindNode::Type::literal;
        node.literal = fold(literal);
        return make(std::move(node), error);
    }

    char class_character(std::string& error) {
        if (position_ >= input_.size()) {
            error = "unclosed character class";
            return 0;
        }
        char value = input_[position_++];
        if (value == '\\') {
            if (position_ >= input_.size()) {
                error = "dangling class escape";
                return 0;
            }
            value = input_[position_++];
        }
        return fold(value);
    }

    std::shared_ptr<const FindNode> parse_class(std::string& error) {
        FindNode node;
        node.type = FindNode::Type::character_class;
        if (position_ < input_.size() && input_[position_] == '^') {
            node.negated = true;
            ++position_;
        }
        bool has_entry = false;
        while (position_ < input_.size() && input_[position_] != ']') {
            const char begin = class_character(error);
            if (!error.empty()) {
                return {};
            }
            char end = begin;
            if (position_ + 1u < input_.size() && input_[position_] == '-' &&
                input_[position_ + 1u] != ']') {
                ++position_;
                end = class_character(error);
                if (!error.empty() || begin > end) {
                    error = "invalid character range";
                    return {};
                }
            }
            node.ranges.emplace_back(begin, end);
            has_entry = true;
        }
        if (!has_entry || position_ >= input_.size() || input_[position_] != ']') {
            error = "empty or unclosed character class";
            return {};
        }
        ++position_;
        return make(std::move(node), error);
    }

    std::string_view input_;
    std::size_t position_{0};
    std::size_t nodes_{0};
};

class AttributeParser {
public:
    explicit AttributeParser(std::string_view input) : input_(input) {}

    std::shared_ptr<const FindAttributeNode> parse(std::string& error) {
        auto result = parse_or(error, 0);
        skip_space();
        if (!result || position_ != input_.size()) {
            if (error.empty()) {
                error = "unexpected attribute-expression token";
            }
            return {};
        }
        return result;
    }

private:
    std::shared_ptr<const FindAttributeNode> make(FindAttributeNode node,
                                                  std::string& error) {
        if (++nodes_ > 256) {
            error = "attribute expression is too complex";
            return {};
        }
        return std::make_shared<FindAttributeNode>(std::move(node));
    }

    void skip_space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(std::string_view token) {
        skip_space();
        if (input_.substr(position_, token.size()) != token) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    std::shared_ptr<const FindAttributeNode> parse_or(std::string& error,
                                                      std::size_t depth) {
        auto left = parse_and(error, depth);
        if (!left) {
            return {};
        }
        while (consume("||")) {
            auto right = parse_and(error, depth);
            if (!right) {
                return {};
            }
            FindAttributeNode node;
            node.type = FindAttributeNode::Type::logical_or;
            node.children = {std::move(left), std::move(right)};
            left = make(std::move(node), error);
        }
        return left;
    }

    std::shared_ptr<const FindAttributeNode> parse_and(std::string& error,
                                                       std::size_t depth) {
        auto left = parse_factor(error, depth);
        if (!left) {
            return {};
        }
        while (consume("&&")) {
            auto right = parse_factor(error, depth);
            if (!right) {
                return {};
            }
            FindAttributeNode node;
            node.type = FindAttributeNode::Type::logical_and;
            node.children = {std::move(left), std::move(right)};
            left = make(std::move(node), error);
        }
        return left;
    }

    std::shared_ptr<const FindAttributeNode> parse_factor(std::string& error,
                                                          std::size_t depth) {
        if (depth > 32) {
            error = "attribute-expression nesting is too deep";
            return {};
        }
        if (consume("!")) {
            auto child = parse_factor(error, depth + 1u);
            if (!child) {
                return {};
            }
            FindAttributeNode node;
            node.type = FindAttributeNode::Type::logical_not;
            node.children = {std::move(child)};
            return make(std::move(node), error);
        }
        if (consume("(")) {
            auto child = parse_or(error, depth + 1u);
            if (!child || !consume(")")) {
                if (error.empty()) {
                    error = "unclosed attribute-expression group";
                }
                return {};
            }
            return child;
        }
        return parse_relation(error);
    }

    static std::optional<FindAttributeNode::Attribute> attribute_id(
        std::string_view identifier) {
        std::string name(identifier);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (name == "VI_ATTR_INTF_TYPE") {
            return FindAttributeNode::Attribute::intf_type;
        }
        if (name == "VI_ATTR_INTF_NUM") {
            return FindAttributeNode::Attribute::intf_num;
        }
        if (name == "VI_ATTR_RSRC_CLASS") {
            return FindAttributeNode::Attribute::rsrc_class;
        }
        if (name == "VI_ATTR_RSRC_NAME") {
            return FindAttributeNode::Attribute::rsrc_name;
        }
        if (name == "VI_ATTR_ASRL_BAUD") {
            return FindAttributeNode::Attribute::asrl_baud;
        }
        return std::nullopt;
    }

    std::optional<std::string_view> identifier() {
        skip_space();
        const auto start = position_;
        if (position_ >= input_.size() ||
            std::isalpha(static_cast<unsigned char>(input_[position_])) == 0) {
            return std::nullopt;
        }
        ++position_;
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_]);
            if (std::isalnum(character) == 0 && character != '_') {
                break;
            }
            ++position_;
        }
        return input_.substr(start, position_ - start);
    }

    std::optional<FindAttributeNode::Compare> compare_operator() {
        if (consume("==")) {
            return FindAttributeNode::Compare::equal;
        }
        if (consume("!=")) {
            return FindAttributeNode::Compare::not_equal;
        }
        if (consume(">=")) {
            return FindAttributeNode::Compare::greater_equal;
        }
        if (consume("<=")) {
            return FindAttributeNode::Compare::less_equal;
        }
        if (consume(">")) {
            return FindAttributeNode::Compare::greater;
        }
        if (consume("<")) {
            return FindAttributeNode::Compare::less;
        }
        return std::nullopt;
    }

    std::optional<std::string> string_literal(std::string& error) {
        skip_space();
        if (position_ >= input_.size() || input_[position_] != '"') {
            return std::nullopt;
        }
        ++position_;
        const auto start = position_;
        while (position_ < input_.size() && input_[position_] != '"') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            error = "unclosed attribute string";
            return std::nullopt;
        }
        std::string value(input_.substr(start, position_ - start));
        ++position_;
        return value;
    }

    std::optional<std::int64_t> number_literal(std::string& error) {
        skip_space();
        const auto start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        int base = 10;
        if (position_ + 1u < input_.size() && input_[position_] == '0' &&
            (input_[position_ + 1u] == 'x' || input_[position_ + 1u] == 'X')) {
            if (position_ != start) {
                error = "negative hexadecimal attribute value is not supported";
                return std::nullopt;
            }
            base = 16;
            position_ += 2u;
        }
        const auto digits = position_;
        while (position_ < input_.size() &&
               (base == 16 ? std::isxdigit(static_cast<unsigned char>(input_[position_]))
                           : std::isdigit(static_cast<unsigned char>(input_[position_]))) != 0) {
            ++position_;
        }
        if (position_ == digits) {
            return std::nullopt;
        }
        std::int64_t value = 0;
        const auto first = input_.data() + static_cast<std::ptrdiff_t>(base == 16 ? digits : start);
        const auto last = input_.data() + static_cast<std::ptrdiff_t>(position_);
        const auto converted = std::from_chars(first, last, value, base);
        if (converted.ec != std::errc{} || converted.ptr != last) {
            error = "attribute number is out of range";
            return std::nullopt;
        }
        return value;
    }

    std::shared_ptr<const FindAttributeNode> parse_relation(std::string& error) {
        const auto raw_identifier = identifier();
        if (!raw_identifier) {
            error = "missing attribute identifier";
            return {};
        }
        const auto attribute = attribute_id(*raw_identifier);
        if (!attribute) {
            error = "unsupported or non-global attribute";
            return {};
        }
        const auto comparison = compare_operator();
        if (!comparison) {
            error = "missing attribute comparison operator";
            return {};
        }
        FindAttributeNode node;
        node.attribute = *attribute;
        node.compare = *comparison;
        const bool expects_string = *attribute == FindAttributeNode::Attribute::rsrc_class ||
                                    *attribute == FindAttributeNode::Attribute::rsrc_name;
        if (expects_string) {
            if (*comparison != FindAttributeNode::Compare::equal &&
                *comparison != FindAttributeNode::Compare::not_equal) {
                error = "string attributes only support == and !=";
                return {};
            }
            auto value = string_literal(error);
            if (!value) {
                if (error.empty()) {
                    error = "string attribute requires a quoted value";
                }
                return {};
            }
            node.string_value = true;
            node.text = std::move(*value);
        } else {
            auto value = number_literal(error);
            if (!value) {
                if (error.empty()) {
                    error = "numeric attribute requires a number";
                }
                return {};
            }
            node.number = *value;
        }
        return make(std::move(node), error);
    }

    std::string_view input_;
    std::size_t position_{0};
    std::size_t nodes_{0};
};

using Positions = std::set<std::size_t>;

Positions match_node(const FindNode& node, std::string_view value, std::size_t position);

Positions repeat_closure(const FindNode& child, std::string_view value,
                         const Positions& initial) {
    Positions reached = initial;
    Positions frontier = initial;
    while (!frontier.empty()) {
        Positions next;
        for (const auto position : frontier) {
            for (const auto candidate : match_node(child, value, position)) {
                if (reached.insert(candidate).second) {
                    next.insert(candidate);
                }
            }
        }
        frontier = std::move(next);
    }
    return reached;
}

Positions match_node(const FindNode& node, std::string_view value, std::size_t position) {
    switch (node.type) {
        case FindNode::Type::empty:
            return {position};
        case FindNode::Type::literal:
            if (position < value.size() && fold(value[position]) == node.literal) {
                return {position + 1u};
            }
            return {};
        case FindNode::Type::any:
            return position < value.size() ? Positions{position + 1u} : Positions{};
        case FindNode::Type::character_class: {
            if (position >= value.size()) {
                return {};
            }
            const char character = fold(value[position]);
            const bool contained = std::any_of(
                node.ranges.begin(), node.ranges.end(), [character](const auto& range) {
                    return range.first <= character && character <= range.second;
                });
            return contained != node.negated ? Positions{position + 1u} : Positions{};
        }
        case FindNode::Type::concatenation: {
            Positions positions{position};
            for (const auto& child : node.children) {
                Positions next;
                for (const auto current : positions) {
                    const auto matched = match_node(*child, value, current);
                    next.insert(matched.begin(), matched.end());
                }
                positions = std::move(next);
                if (positions.empty()) {
                    break;
                }
            }
            return positions;
        }
        case FindNode::Type::alternate: {
            Positions positions;
            for (const auto& child : node.children) {
                const auto matched = match_node(*child, value, position);
                positions.insert(matched.begin(), matched.end());
            }
            return positions;
        }
        case FindNode::Type::repeat: {
            if (!node.one_or_more) {
                return repeat_closure(*node.children.front(), value, Positions{position});
            }
            const auto first = match_node(*node.children.front(), value, position);
            return repeat_closure(*node.children.front(), value, first);
        }
    }
    return {};
}

bool compare_number(std::int64_t left, FindAttributeNode::Compare comparison,
                    std::int64_t right) {
    switch (comparison) {
        case FindAttributeNode::Compare::equal:
            return left == right;
        case FindAttributeNode::Compare::not_equal:
            return left != right;
        case FindAttributeNode::Compare::less:
            return left < right;
        case FindAttributeNode::Compare::less_equal:
            return left <= right;
        case FindAttributeNode::Compare::greater:
            return left > right;
        case FindAttributeNode::Compare::greater_equal:
            return left >= right;
    }
    return false;
}

bool compare_string(std::string_view left, FindAttributeNode::Compare comparison,
                    std::string_view right) {
    if (comparison == FindAttributeNode::Compare::equal) {
        return left == right;
    }
    if (comparison == FindAttributeNode::Compare::not_equal) {
        return left != right;
    }
    return false;
}

bool match_attributes(const FindAttributeNode& node,
                      const ResourceDescriptor& resource) {
    switch (node.type) {
        case FindAttributeNode::Type::logical_and:
            return match_attributes(*node.children[0], resource) &&
                   match_attributes(*node.children[1], resource);
        case FindAttributeNode::Type::logical_or:
            return match_attributes(*node.children[0], resource) ||
                   match_attributes(*node.children[1], resource);
        case FindAttributeNode::Type::logical_not:
            return !match_attributes(*node.children[0], resource);
        case FindAttributeNode::Type::relation:
            break;
    }
    switch (node.attribute) {
        case FindAttributeNode::Attribute::intf_type:
            return compare_number(resource.interface_type, node.compare, node.number);
        case FindAttributeNode::Attribute::intf_num:
            return compare_number(resource.interface_number, node.compare, node.number);
        case FindAttributeNode::Attribute::rsrc_class:
            return compare_string(resource.resource_class, node.compare, node.text);
        case FindAttributeNode::Attribute::rsrc_name:
            return compare_string(resource.canonical_name, node.compare, node.text);
        case FindAttributeNode::Attribute::asrl_baud:
            return resource.kind == ResourceKind::asrl_instr &&
                   compare_number(9600, node.compare, node.number);
    }
    return false;
}

}  // namespace

std::optional<FindExpression> FindExpression::compile(std::string_view expression,
                                                       std::string& error) {
    error.clear();
    if (expression.empty() || expression.size() >= VI_FIND_BUFLEN) {
        error = "empty or oversized expression";
        return std::nullopt;
    }
    std::string_view regular = expression;
    std::string_view attributes;
    const auto opening = expression.find('{');
    if (opening != std::string_view::npos) {
        if (opening == 0 || expression.back() != '}' ||
            expression.find('{', opening + 1u) != std::string_view::npos ||
            expression.find('}') != expression.size() - 1u) {
            error = "malformed attribute-filter delimiter";
            return std::nullopt;
        }
        regular = expression.substr(0, opening);
        attributes = expression.substr(opening + 1u,
                                       expression.size() - opening - 2u);
        if (attributes.empty()) {
            error = "empty attribute expression";
            return std::nullopt;
        }
    } else if (expression.find('}') != std::string_view::npos) {
        error = "malformed attribute-filter delimiter";
        return std::nullopt;
    }
    Parser parser(regular);
    auto root = parser.parse(error);
    if (!root) {
        return std::nullopt;
    }
    std::shared_ptr<const FindAttributeNode> attribute_root;
    if (!attributes.empty()) {
        AttributeParser attribute_parser(attributes);
        attribute_root = attribute_parser.parse(error);
        if (!attribute_root) {
            return std::nullopt;
        }
    }
    return FindExpression(std::move(root), std::move(attribute_root));
}

bool FindExpression::matches(std::string_view value) const {
    const auto positions = match_node(*root_, value, 0);
    if (!positions.contains(value.size())) {
        return false;
    }
    if (!attributes_) {
        return true;
    }
    const auto resource = parse_resource(value);
    return resource && match_attributes(*attributes_, *resource);
}

bool FindExpression::matches(const ResourceDescriptor& resource) const {
    const auto positions = match_node(*root_, resource.canonical_name, 0);
    return positions.contains(resource.canonical_name.size()) &&
           (!attributes_ || match_attributes(*attributes_, resource));
}

}  // namespace wrvisa
