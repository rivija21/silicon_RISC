#include "Backend.h"

namespace sp {

const LinkedInstruction* Executable::instruction(uint32_t pc) const {
    if (pc < textBase || (pc & 3) != 0) return nullptr;
    size_t k = (pc - textBase) / 4;
    return k < instructions.size() ? &instructions[k] : nullptr;
}

void Executable::finish() {
    functionStarts.clear();
    for (auto& s : symbols)
        if (s.section == SectionKind::Text && !hasPrefix(s.name, ".L")) functionStarts.push_back({s.address, s.name});
    std::stable_sort(functionStarts.begin(), functionStarts.end(), [](auto& a, auto& b) { return a.first < b.first; });
}

std::optional<std::string> Executable::symbolize(uint32_t addr) const {
    for (auto& s : symbols)
        if (s.address == addr && !hasPrefix(s.name, ".")) return s.name;
    if (addr >= textBase && addr < textEnd()) {
        for (auto it = functionStarts.rbegin(); it != functionStarts.rend(); ++it) {
            if (it->first <= addr) return addr == it->first ? it->second : cat(it->second, "+", addr - it->first);
        }
    }
    if (addr >= dataBase && addr < dataEnd()) {
        const LinkedSymbol* best = nullptr;
        for (auto& s : symbols)
            if (s.section == SectionKind::Data && s.address <= addr && (!best || best->address < s.address)) best = &s;
        if (best) return addr == best->address ? best->name : cat(best->name, "+", addr - best->address);
    }
    return std::nullopt;
}

std::optional<std::string> Executable::label(uint32_t addr) const {
    std::vector<const LinkedSymbol*> here;
    for (auto& s : symbols)
        if (s.address == addr && s.section == SectionKind::Text) here.push_back(&s);
    for (auto* s : here)
        if (hasPrefix(s->name, ".L") && !hasSuffix(s->name, "_return")) return s->name;
    if (!here.empty()) return here.front()->name;
    return std::nullopt;
}

std::optional<std::string> Executable::function(uint32_t pc) const {
    for (auto it = functionStarts.rbegin(); it != functionStarts.rend(); ++it)
        if (it->first <= pc) return it->second;
    return std::nullopt;
}

std::optional<uint32_t> Executable::address(const std::string& name) const {
    for (auto& s : symbols)
        if (s.name == name && s.isGlobal) return s.address;
    for (auto& s : symbols)
        if (s.name == name) return s.address;
    return std::nullopt;
}

/// The linker: picks the library members the program needs, places every
/// section in memory, gives each symbol its final address and patches the
/// relocations the assembler left behind.
std::pair<ExecutableP, std::vector<Diagnostic>> Linker::link(const std::vector<ObjectFileP>& inputs, const std::vector<ObjectFileP>& archive,
                                                             const std::string& entry) {
    std::vector<Diagnostic> diags;
    auto err = [&](const std::string& m) { diags.emplace_back(Diagnostic::Severity::Error, Stage::Linker, m); };

    // 1. Resolve undefined symbols by pulling members out of the library archive.
    std::vector<ObjectFileP> objs = inputs;
    std::map<std::string, std::string> reasons;
    for (auto& o : inputs) reasons[o->name] = "named on the command line";
    std::map<std::string, std::string> defined;
    auto addDefs = [&](const ObjectFile& o) {
        for (auto& g : o.globalSymbols())
            if (!defined.count(g)) defined[g] = o.name;
    };
    for (auto& o : objs) addDefs(*o);
    bool changed = true;
    while (changed) {
        changed = false;
        std::set<std::string> needed;
        for (auto& o : objs)
            for (auto& u : o->undefined)
                if (!defined.count(u)) needed.insert(u);
        for (auto& sym : needed) {
            if (defined.count(sym)) continue;
            ObjectFileP member;
            for (auto& m : archive) {
                auto gs = m->globalSymbols();
                if (std::find(gs.begin(), gs.end(), sym) == gs.end()) continue;
                if (std::find(objs.begin(), objs.end(), m) != objs.end()) continue;
                member = m;
                break;
            }
            if (!member) continue;
            std::string who = "?";
            for (auto& o : objs)
                if (o->undefined.count(sym)) {
                    who = o->name;
                    break;
                }
            reasons[member->name] = cat(who, " needs ", sym);
            objs.push_back(member);
            addDefs(*member);
            changed = true;
        }
    }
    // Duplicate definitions.
    std::map<std::string, std::string> seen;
    for (auto& o : objs) {
        for (auto& g : o->globalSymbols()) {
            auto f = seen.find(g);
            if (f != seen.end()) err(cat("multiple definition of '", g, "' (in ", f->second, " and ", o->name, ")"));
            seen[g] = o->name;
        }
    }
    for (auto& o : objs)
        for (auto& u : o->undefined)
            if (!defined.count(u)) err(cat("undefined reference to '", u, "' (used in ", o->name, ")"));
    if (!diags.empty()) return {nullptr, diags};

    // 2. Lay out the sections.
    auto exe = std::make_shared<Executable>();
    std::map<const ObjectFile*, uint32_t> textAt, dataAt;
    uint32_t tpos = exe->textBase, dpos = exe->dataBase;
    for (auto& o : objs) {
        textAt[o.get()] = tpos;
        dpos = (dpos + 7) & ~7u;
        dataAt[o.get()] = dpos;
        exe->objects.push_back(LinkedObject{o->name, tpos, (int)o->text.size(), dpos, (int)o->data.size(), o->isLibrary,
                                            reasons.count(o->name) ? reasons[o->name] : "", o->summary});
        tpos += (uint32_t)o->text.size();
        dpos += (uint32_t)o->data.size();
    }
    for (auto& o : objs) exe->text.insert(exe->text.end(), o->text.begin(), o->text.end());
    exe->data.assign(dpos - exe->dataBase, 0);
    for (auto& o : objs) {
        size_t base = dataAt[o.get()] - exe->dataBase;
        std::copy(o->data.begin(), o->data.end(), exe->data.begin() + base);
    }

    // 3. Symbol addresses.
    std::map<std::string, uint32_t> globalAddr;
    for (auto& o : objs) {
        for (auto& [n, s] : o->symbols) {
            uint32_t base = s.section == SectionKind::Text ? textAt[o.get()] : dataAt[o.get()];
            uint32_t a = base + (uint32_t)s.offset;
            exe->symbols.push_back(LinkedSymbol{s.name, a, s.section, o->name, s.isGlobal});
            if (s.isGlobal) globalAddr[s.name] = a;
        }
    }
    std::stable_sort(exe->symbols.begin(), exe->symbols.end(), [](const LinkedSymbol& a, const LinkedSymbol& b) {
        return a.address != b.address ? a.address < b.address : a.name < b.name;
    });
    auto resolve = [&](const std::string& name, const ObjectFile& o) -> std::optional<uint32_t> {
        auto it = o.symbols.find(name);
        if (it != o.symbols.end()) {
            uint32_t base = it->second.section == SectionKind::Text ? textAt[&o] : dataAt[&o];
            return base + (uint32_t)it->second.offset;
        }
        auto g = globalAddr.find(name);
        if (g != globalAddr.end()) return g->second;
        return std::nullopt;
    };

    // 4. Listing with final addresses.
    for (auto& o : objs) {
        uint32_t base = textAt[o.get()];
        for (auto& e : o->listing) {
            LinkedInstruction li;
            li.address = base + (uint32_t)e.offset;
            li.word = e.word;
            li.decoded = Decoder::decode(e.word);
            li.text = e.text;
            li.pseudo = e.pseudo;
            li.part = e.part;
            li.parts = e.parts;
            li.object = o->name;
            li.asmLine = e.asmLine;
            li.origin = e.origin;
            li.function = e.function;
            li.unlinkedWord = e.word;
            exe->instructions.push_back(std::move(li));
        }
        uint32_t dbase = dataAt[o.get()];
        for (auto& d : o->dataListing) exe->dataEntries.push_back(LinkedData{dbase + (uint32_t)d.offset, d.size, d.text, o->name, d.origin});
    }
    std::stable_sort(exe->instructions.begin(), exe->instructions.end(),
                     [](const LinkedInstruction& a, const LinkedInstruction& b) { return a.address < b.address; });

    // 5. Apply relocations.
    auto word = [&](uint32_t a) {
        size_t k = a - exe->textBase;
        return (uint32_t)exe->text[k] | (uint32_t)exe->text[k + 1] << 8 | (uint32_t)exe->text[k + 2] << 16 | (uint32_t)exe->text[k + 3] << 24;
    };
    auto setWord = [&](uint32_t a, uint32_t w, bool data) {
        std::vector<uint8_t>& mem = data ? exe->data : exe->text;
        size_t k = a - (data ? exe->dataBase : exe->textBase);
        for (int i = 0; i < 4; i++) mem[k + i] = (w >> (8 * i)) & 0xFF;
    };
    for (auto& o : objs) {
        uint32_t tbase = textAt[o.get()], dbase = dataAt[o.get()];
        for (auto& rl : o->relocations) {
            auto s = resolve(rl.symbol, *o);
            if (!s) {
                err(cat("undefined reference to '", rl.symbol, "' (used in ", o->name, ")"));
                continue;
            }
            uint32_t target = *s + (uint32_t)rl.addend;
            uint32_t p = (rl.section == SectionKind::Text ? tbase : dbase) + (uint32_t)rl.offset;
            if (rl.type == RelocType::Abs32) {
                setWord(p, target, rl.section == SectionKind::Data);
                exe->relocations.push_back(RelocationRecord{o->name, rl.type, rl.symbol, p, target, 0, target, (int32_t)target});
                continue;
            }
            uint32_t before = word(p);
            int32_t imm = 0;
            switch (rl.type) {
            case RelocType::Branch: case RelocType::Jal: {
                imm = (int32_t)(target - p);
                int32_t limit = rl.type == RelocType::Branch ? 4096 : 1 << 20;
                if (imm < -limit || imm >= limit) err(cat(o->name, ": relocation ", relocName(rl.type), " to '", rl.symbol, "' is out of range"));
                break;
            }
            case RelocType::PcrelHi20: {
                int32_t v = (int32_t)(target - p);
                imm = (RV::add(v, 0x800) >> 12) & 0xFFFFF;
                break;
            }
            case RelocType::PcrelLo12I: case RelocType::PcrelLo12S: {
                uint32_t hiP = tbase + (uint32_t)(rl.hiOffset ? *rl.hiOffset : rl.offset);
                int32_t v = (int32_t)(target - hiP);
                imm = RV::sub(v, (int32_t)((uint32_t)(RV::add(v, 0x800) >> 12) << 12));
                break;
            }
            case RelocType::Hi20: imm = (RV::add((int32_t)target, 0x800) >> 12) & 0xFFFFF; break;
            case RelocType::Lo12I: case RelocType::Lo12S: imm = (int32_t)(target << 20) >> 20; break;
            case RelocType::Abs32: imm = 0; break;
            }
            auto after = Encoder::patch(before, imm);
            if (!after) {
                err(cat(o->name, ": cannot patch the instruction at ", Format::hex(p)));
                continue;
            }
            setWord(p, *after, false);
            exe->relocations.push_back(RelocationRecord{o->name, rl.type, rl.symbol, p, target, before, *after, imm});
            for (auto& ins : exe->instructions)
                if (ins.address == p) {
                    ins.word = *after;
                    ins.decoded = Decoder::decode(*after);
                    break;
                }
        }
    }
    auto e = globalAddr.find(entry);
    if (e == globalAddr.end()) {
        err(cat("no entry point '", entry, "'"));
        return {nullptr, diags};
    }
    exe->entry = e->second;
    exe->finish();
    bool hasError = std::any_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.severity == Diagnostic::Severity::Error; });
    return {hasError ? nullptr : exe, diags};
}

}  // namespace sp
