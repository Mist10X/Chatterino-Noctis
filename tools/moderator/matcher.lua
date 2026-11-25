-- Noctis Moderator module. Authorized updates are signed by its owner.
-- Автор Mist1X прямо запрещает несанкционированную модификацию модуля
-- и обход ограничений доступа. Изменения разрешены только владельцем.
-- Inputs are normalized text and enabled rules for the verified channel.
-- Return only an index; the trusted host validates and sends the reply.
return function(text, rules)
    local best, best_length = nil, nil
    for i = 1, #rules do
        local rule = rules[i]
        for j = 1, #rule.triggers do
            local trigger = rule.triggers[j]
            if rule.exact then
                if text == trigger then
                    return i
                end
            elseif string.find(text, trigger, 1, true) then
                if best_length == nil or #trigger < best_length then
                    best, best_length = i, #trigger
                end
            end
        end
    end
    return best
end
