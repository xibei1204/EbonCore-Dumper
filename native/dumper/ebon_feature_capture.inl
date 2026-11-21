// Included inside resources::Dumper. All reads use its validated reflection
// inventory; this capture never loads assets or invokes game functions.
static std::string lower(std::string value){
    for(auto& c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::optional<std::uint32_t> objectFlags(const Object& object)const{
    std::uint32_t flags=0;
    if(!reflection_.read(object.address,reflection_.layout().objFlags,flags))return {};
    return flags;
}
bool templateObject(const Object& object)const{
    const auto flags=objectFlags(object);
    return object.name.rfind("Default__",0)==0||(flags&&(*flags&0x30u));
}
bool instance(const Object& object)const{
    return !types_.count(object.address)&&!templateObject(object);
}
bool containsField(const Type* type,const std::vector<std::string>& names)const{
    std::set<std::string> seen;
    for(;type&&seen.insert(type->path).second;){
        for(const auto& field:type->fields)
            if(std::find(names.begin(),names.end(),lower(field.name))!=names.end())return true;
        auto parent=paths_.find(type->super);type=parent==paths_.end()?nullptr:parent->second;
    }
    return false;
}
bool featureAsset(const Object& object,const Type& type)const{
    return instance(object)&&(isA(&type,"/Script/HTGame.SystematicGameFeaturesDataAsset")||
        lower(object.name)=="da_systematicgamefeatures");
}
bool featureTable(const Object& object,const Type& type)const{
    if(!instance(object)||!isA(&type,"/Script/Engine.DataTable"))return false;
    const auto name=lower(object.name);
    return name.find("systematicgamefeatures")!=name.npos||name.find("functionunlock")!=name.npos||
           name.find("mainformvisibility")!=name.npos;
}
bool featureComponent(const Object& object,const Type& type)const{
    return instance(object)&&isA(&type,"/Script/HTGame.SystematicGameFeaturesComponent");
}
bool menuBinding(const Object& object,const Type& type)const{
    return instance(object)&&containsField(&type,{"gamefeatureid","functionunlockid"});
}
bool battleIdentity(const std::string& identity)const{
    const auto s=lower(identity);
    for(const char* name:{"character","avatar","skill","ability","gameplayeffect","weapon","combat","role"})
        if(s.find(name)!=s.npos)return true;
    return false;
}
bool battleResource(const Object& object,const Type& type)const{
    if(types_.count(object.address))return false;
    return (object.path.rfind("/Game/",0)==0||isA(&type,"/Script/HTGame.HTPlayerCharacter")||
            isA(&type,"/Script/GameplayAbilities.AbilitySystemComponent"))&&
           battleIdentity(object.path+" "+type.path+" "+type.super);
}
const Object* playerOwner(const Object& object)const{
    std::set<std::uint64_t> seen;const Object* cursor=&object;
    for(unsigned depth=0;cursor&&depth<32;++depth){
        std::uint64_t address=0;
        if(!reflection_.read(cursor->address,reflection_.layout().objOuter,address)||!seen.insert(address).second)break;
        auto parent=objects_.find(address);if(parent==objects_.end()||!current(*parent->second))break;
        cursor=parent->second;auto type=types_.find(cursor->klass);if(type==types_.end())break;
        for(const auto* base:{"/Script/Engine.PlayerController","/Script/Engine.PlayerState","/Script/Engine.Pawn",
                             "/Script/Engine.LocalPlayer","/Script/HTGame.HTPlayerController","/Script/HTGame.HTPlayerState"})
            if(isA(type->second,base))return cursor;
    }
    return nullptr;
}
std::string objectMeta(const Object& object,const Type& type)const{
    std::uint64_t outer=0;const bool outerRead=reflection_.read(object.address,reflection_.layout().objOuter,outer);
    auto parent=objects_.find(outer);const auto flags=objectFlags(object);const auto player=playerOwner(object);
    const auto body=capturedBodies_.find(object.path);
    return "{\"object\":"+quote(object.path)+",\"name\":"+quote(object.name)+",\"class\":"+quote(type.path)+
        ",\"address\":"+quote(hex(object.address))+",\"outer\":"+
        (outerRead&&parent!=objects_.end()?quote(parent->second->path):"null")+
        ",\"owner_player\":"+(player?quote(player->path):"null")+
        ",\"object_flags\":"+(flags?quote(hex(*flags)):"null")+
        ",\"is_default\":"+(templateObject(object)?"true":"false")+
        ",\"is_default_subobject\":"+(flags?((*flags&0x40000u)?"true":"false"):"null")+
        ",\"body_file\":"+(body==capturedBodies_.end()?"null":quote(body->second.file))+
        ",\"records\":"+(body==capturedBodies_.end()?"0":std::to_string(body->second.rows))+
        ",\"unresolved\":"+(body==capturedBodies_.end()?"null":std::to_string(body->second.issues))+
        ",\"status\":"+quote(body==capturedBodies_.end()?"not_exported":body->second.issues?"partial":"captured")+"}";
}
void assetReference(const std::string& target,const std::string& kind){
    if(captureReferences_&&!target.empty())
        assetReferences_.push_back({captureRoot_,location_,target,kind});
}
const Property* fieldNamed(const Type& type,const char* name)const{
    for(const auto& field:type.fields)if(field.name==name)return &field;
    return nullptr;
}
std::string assetPath(std::uint64_t address,const Type& type)const{
    // Decode by reflected field offsets, rather than matching a JSON
    // substring against the names of already-loaded objects.
    const auto asset=fieldNamed(type,"AssetPath"),sub=fieldNamed(type,"SubPathString");
    if(!asset||!sub||asset->type!="StructProperty")return {};
    const auto top=paths_.find(asset->target);if(top==paths_.end())return {};
    const auto package=fieldNamed(*top->second,"PackageName"),name=fieldNamed(*top->second,"AssetName");
    if(!package||!name||package->type!="NameProperty"||name->type!="NameProperty")return {};
    const auto packageName=reflection_.name(address+asset->offset+package->offset,0);
    const auto objectName=reflection_.name(address+asset->offset+name->offset,0);
    if(packageName=="None"&&objectName=="None")return "None";
    if(packageName.empty()||packageName[0]!='/'||objectName.empty()||objectName=="None")return {};
    std::string suffix;
    if(sub->type=="StrProperty"){if(!string(address+sub->offset,suffix))return {};}
    else if(sub->type=="Utf8StrProperty"){if(!utf8String(address+sub->offset,suffix))return {};}
    else return {};
    return packageName+"."+objectName+(suffix.empty()?"":":"+suffix);
}
bool utf8String(std::uint64_t address,std::string& result)const{
    ArrayHeader h;
    if(!memory_.get(address,h)||h.num<0||h.num>maxString_||h.capacity<h.num||h.capacity>maxString_*16)return false;
    if(!h.num){result.clear();return true;}
    if(h.data<0x10000)return false;
    result.resize(h.num);
    if(!memory_.copy(h.data,result.data(),result.size())||result.back()!=0)return false;
    result.pop_back();if(result.find('\0')!=result.npos)return false;
    try{wide(result);}catch(...){return false;}
    ArrayHeader after;return memory_.get(address,after)&&h==after;
}
void exportObject(const Object& object){
    if(capturedBodies_.count(object.path)||types_.count(object.address))return;
    auto it=types_.find(object.klass);if(it==types_.end()||it->second->kind!="class")return;
    const auto& type=*it->second;
    if(!current(object)){unavailable("stale resource object: "+object.path,object.address);return;}
    captureRoot_=object.path;
    if(isA(&type,"/Script/Engine.DataTable")){if(!templateObject(object))table(object,type);}
    else if(isA(&type,"/Script/Engine.StringTable")){if(!templateObject(object))stringTable(object);}
    else {
        const auto kind=isA(&type,"/Script/Engine.DataAsset")?"DataAsset":extraKind(object,type);
        if(!kind.empty())objectValues(object,type,kind);
    }
}
void capturePriorities(){
    if(!config_.number("resource_feature_exports",1))return;
    const auto menuMax=(std::size_t)option("resource_feature_max_objects",4096,1,65536);
    const auto battleMax=(std::size_t)option("resource_battle_max_objects",4096,1,65536);
    const auto maxDepth=(unsigned)option("resource_dependency_depth",8,1,64);
    std::map<std::string,const Object*> byPath;
    std::map<std::uint64_t,const Object*> defaults;
    std::vector<const Object*> menu;
    std::array<std::vector<const Object*>,3> battle;
    for(const auto& object:reflection_.objects){
        checkpoint();
        byPath[object.path]=&object;
        if(object.name.rfind("Default__",0)==0)defaults[object.klass]=&object;
        auto type=types_.find(object.klass);if(type==types_.end()||types_.count(object.address))continue;
        if(featureAsset(object,*type->second)||featureTable(object,*type->second)||featureComponent(object,*type->second)||
           menuBinding(object,*type->second)||containsField(type->second,{"functionunlockdatatable","mainformvisibilityconfigdatatable"}))
            menu.push_back(&object);
        if(battleResource(object,*type->second)){
            const auto rank=instance(object)&&(isA(type->second,"/Script/Engine.DataTable")||
                isA(type->second,"/Script/Engine.DataAsset"))?0:templateObject(object)?1:2;
            battle[rank].push_back(&object);
        }
    }
    captureReferences_=true;
    std::map<std::string,std::vector<std::size_t>> referenceIndices;
    struct Stage {std::size_t visited=0,depthLimited=0;bool limited=false;};
    const auto runStage=[&](const std::vector<const Object*>& seeds,std::size_t limit,bool battleStage){
        Stage stage;
        std::vector<std::pair<const Object*,unsigned>> queue;std::set<std::string> queued;
        const auto enqueue=[&](const Object& object,unsigned depth){
            if(types_.count(object.address)||queued.count(object.path))return;
            if(queue.size()>=limit){stage.limited=true;return;}
            queued.insert(object.path);queue.push_back({&object,depth});
        };
        std::size_t next=0;
        for(const auto seed:seeds){
            // Drain dependencies before the next broad seed can take their slots.
            enqueue(*seed,0);
            for(;next<queue.size();++next){
                checkpoint();const auto object=queue[next].first;const auto depth=queue[next].second;
                if(battleStage)battleObjects_.insert(object->path);
                const auto before=assetReferences_.size();exportObject(*object);
                const auto end=assetReferences_.size();
                for(auto i=before;i<end;++i)referenceIndices[assetReferences_[i].root].push_back(i);
                for(const auto i:referenceIndices[object->path]){
                    auto found=byPath.find(assetReferences_[i].target);if(found==byPath.end())continue;
                    auto target=found->second;
                    if(types_.count(target->address)){
                        const auto cdo=defaults.find(target->address);if(cdo==defaults.end())continue;target=cdo->second;
                    }
                    // Previously exported menu dependencies still participate
                    // in the battle closure, including their own references.
                    if(queued.count(target->path))continue;
                    if(depth>=maxDepth){++stage.depthLimited;continue;}
                    enqueue(*target,depth+1);
                }
            }
        }
        stage.visited=queue.size();return stage;
    };
    const auto menuStage=runStage(menu,menuMax,false);
    writeFeatureReports(menuStage.limited);
    std::vector<const Object*> battleSeeds;
    for(const auto& tier:battle)battleSeeds.insert(battleSeeds.end(),tier.begin(),tier.end());
    const auto battleStage=runStage(battleSeeds,battleMax,true);
    captureReferences_=false;
    writeBattleReport();
    std::ofstream plan(output_/"feature_exports"/"priority_capture.json",std::ios::binary);
    if(!plan)throw std::runtime_error("cannot open priority capture report");
    const auto stageJson=[](const Stage& stage,std::size_t seeds,std::size_t limit){
        return "{\"candidates\":"+std::to_string(seeds)+",\"visited\":"+std::to_string(stage.visited)+
            ",\"object_limit\":"+std::to_string(limit)+",\"object_limit_reached\":"+(stage.limited?"true":"false")+
            ",\"depth_limited_references\":"+std::to_string(stage.depthLimited)+"}";
    };
    line(plan,"{\"format\":\"EbonPriorityCapture-v1\",\"dependency_depth\":"+std::to_string(maxDepth)+
        ",\"menu\":"+stageJson(menuStage,menu.size(),menuMax)+",\"battle\":"+stageJson(battleStage,battleSeeds.size(),battleMax)+
        ",\"battle_config_candidates\":"+std::to_string(battle[0].size())+"}");
    plan.close();if(!plan)throw std::runtime_error("cannot finalize priority capture report");
}
void writeFeatureReports(bool limited){
    const auto folder=output_/"feature_exports";std::filesystem::create_directories(folder);
    for(const auto* name:{"systematic_game_features.json","feature_table_rows.jsonl"})
        if(std::filesystem::exists(folder/name))throw std::runtime_error("feature output already exists");
    std::ofstream report(folder/"systematic_game_features.json",std::ios::binary);
    std::ofstream rows(folder/"feature_table_rows.jsonl",std::ios::binary);
    if(!report||!rows)throw std::runtime_error("cannot open feature reports");
    std::vector<const Object*> roots,tables,components,buttons;
    std::set<std::string> tablePaths;
    std::map<std::string,const Object*> byPath;
    for(const auto& object:reflection_.objects)byPath[object.path]=&object;
    for(const auto& object:reflection_.objects){
        auto type=types_.find(object.klass);if(type==types_.end()||types_.count(object.address))continue;
        if(featureAsset(object,*type->second))roots.push_back(&object);
        if(featureTable(object,*type->second))tablePaths.insert(object.path);
        if(featureComponent(object,*type->second))components.push_back(&object);
        if(menuBinding(object,*type->second))buttons.push_back(&object);
    }
    for(const auto& ref:assetReferences_)
        if(ref.source.find(".DataTables")!=ref.source.npos||ref.source.find(".LoadedDataTables")!=ref.source.npos||
           ref.source.find(".FunctionUnlockDataTable")!=ref.source.npos||ref.source.find(".MainFormVisibilityConfigDataTable")!=ref.source.npos)
            tablePaths.insert(ref.target);
    for(const auto& path:tablePaths){
        auto object=byPath.find(path);if(object==byPath.end())continue;
        auto type=types_.find(object->second->klass);
        if(type!=types_.end()&&isA(type->second,"/Script/Engine.DataTable")&&instance(*object->second))tables.push_back(object->second);
    }
    std::string json="{\"format\":\"EbonSystematicGameFeatures-v2\",\"build_sha256\":"+quote(reflection_.moduleSha256)+
        ",\"status\":"+quote(roots.empty()?"not_observed":"observed_partial")+
        ",\"object_limit_reached\":"+(limited?"true":"false")+",\"target_assets\":[";
    const auto appendObjects=[&](const std::vector<const Object*>& objects){
        bool first=true;for(const auto object:objects){if(!first)json+=',';first=false;json+=objectMeta(*object,*types_.at(object->klass));bufferBudget(json);}
    };
    appendObjects(roots);json+="],\"tables\":[";appendObjects(tables);
    json+="],\"component_instances\":[";appendObjects(components);
    json+="],\"menu_buttons\":[";appendObjects(buttons);
    json+="],\"references\":[";
    bool first=true;
    for(const auto& ref:assetReferences_){
        if(!first)json+=',';first=false;
        json+="{\"root\":"+quote(ref.root)+",\"source\":"+quote(ref.source)+",\"target\":"+quote(ref.target)+
            ",\"kind\":"+quote(ref.kind)+",\"target_loaded\":"+(byPath.count(ref.target)?"true":"false")+"}";
        bufferBudget(json);
    }
    json+="],\"missing\":[";
    first=true;const auto missing=[&](const char* reason){if(!first)json+=',';first=false;json+=quote(reason);};
    if(roots.empty())missing("actual_DA_SystematicGameFeatures_not_observed");
    if(components.empty())missing("live_SystematicGameFeaturesComponent_not_observed");
    if(buttons.empty())missing("menu_button_bindings_not_observed");
    if(tables.empty())missing("feature_tables_not_observed");
    for(const auto& ref:assetReferences_)
        if((ref.source.find(".DataTables")!=ref.source.npos||ref.source.find(".FunctionUnlockDataTable")!=ref.source.npos||
            ref.source.find(".MainFormVisibilityConfigDataTable")!=ref.source.npos)&&!capturedBodies_.count(ref.target)){
            if(!first)json+=',';first=false;json+=quote("table_not_exported: "+ref.target);bufferBudget(json);
        }
    json+="],\"coverage\":\"loaded reflected values; body_file retains every field; unreflected native state and unloaded assets remain unavailable\"}";
    line(report,json);
    for(const auto object:tables){
        const auto body=capturedBodies_.find(object->path);if(body==capturedBodies_.end())continue;
        std::ifstream input(output_/body->second.file,std::ios::binary);std::string row;
        if(!input)throw std::runtime_error("captured feature table is missing");
        while(std::getline(input,row)){checkpoint();if(!row.empty())line(rows,"{\"table\":"+quote(object->path)+",\"record\":"+row+"}");}
    }
}
void writeBattleReport(){
    const auto file=output_/"feature_exports"/"battle_resources.jsonl";
    if(std::filesystem::exists(file))throw std::runtime_error("battle resource report already exists");
    std::ofstream battle(file,std::ios::binary);
    if(!battle)throw std::runtime_error("cannot open battle resource report");
    std::set<std::string> loaded;for(const auto& object:reflection_.objects)loaded.insert(object.path);
    // Include actual exported bodies and the exact reference graph. Generated
    // class paths are load targets; their metadata is in reflection/types.jsonl.
    for(const auto& object:reflection_.objects){
        auto type=types_.find(object.klass);if(type==types_.end()||
            (!battleObjects_.count(object.path)&&!battleResource(object,*type->second)))continue;
        line(battle,objectMeta(object,*type->second));
    }
    for(const auto& ref:assetReferences_)
        if(battleObjects_.count(ref.root)||battleIdentity(ref.root)||battleIdentity(ref.target))
            line(battle,"{\"kind\":\"asset_reference\",\"root\":"+quote(ref.root)+",\"source\":"+quote(ref.source)+",\"target\":"+quote(ref.target)+
                ",\"target_loaded\":"+(loaded.count(ref.target)?"true":"false")+"}");
}
