cmake_minimum_required(VERSION 3.25)

set(srgb_linear
    0 304 607 911 1214 1518 1821 2125
    2428 2732 3035 3347 3677 4025 4391 4777
    5182 5605 6049 6512 6995 7499 8023 8568
    9134 9721 10330 10960 11612 12286 12983 13702
    14444 15209 15996 16807 17642 18500 19382 20289
    21219 22174 23153 24158 25187 26241 27321 28426
    29557 30713 31896 33105 34340 35601 36889 38204
    39546 40915 42311 43735 45186 46665 48172 49707
    51269 52861 54480 56128 57805 59511 61246 63010
    64803 66626 68478 70360 72272 74214 76185 78187
    80220 82283 84376 86500 88656 90842 93059 95307
    97587 99899 102242 104616 107023 109462 111932 114435
    116971 119538 122139 124772 127438 130136 132868 135633
    138432 141263 144128 147027 149960 152926 155926 158961
    162029 165132 168269 171441 174647 177888 181164 184475
    187821 191202 194618 198069 201556 205079 208637 212231
    215861 219526 223228 226966 230740 234551 238398 242281
    246201 250158 254152 258183 262251 266356 270498 274677
    278894 283149 287441 291771 296138 300544 304987 309469
    313989 318547 323143 327778 332452 337164 341914 346704
    351533 356400 361307 366253 371238 376262 381326 386429
    391572 396755 401978 407240 412543 417885 423268 428690
    434154 439657 445201 450786 456411 462077 467784 473531
    479320 485150 491021 496933 502886 508881 514918 520996
    527115 533276 539479 545724 552011 558340 564712 571125
    577580 584078 590619 597202 603827 610496 617207 623960
    630757 637597 644480 651406 658375 665387 672443 679542
    686685 693872 701102 708376 715694 723055 730461 737910
    745404 752942 760525 768151 775822 783538 791298 799103
    806952 814847 822786 830770 838799 846873 854993 863157
    871367 879622 887923 896269 904661 913099 921582 930111
    938686 947307 955973 964686 973445 982251 991102 1000000)
list(LENGTH srgb_linear table_length)
if(NOT table_length EQUAL 256)
    message(FATAL_ERROR "sRGB lookup table must contain 256 entries")
endif()

function(luminance color output)
    string(LENGTH "${color}" length)
    if(NOT length EQUAL 7 OR NOT color MATCHES "^#[0-9A-Fa-f]+$")
        message(FATAL_ERROR "Malformed contrast color: ${color}")
    endif()
    foreach(offset IN ITEMS 1 3 5)
        string(SUBSTRING "${color}" ${offset} 2 component)
        math(EXPR index "0x${component}")
        list(GET srgb_linear ${index} "linear_${offset}")
    endforeach()
    math(EXPR result "(2126 * ${linear_1} + 7152 * ${linear_3} + 722 * ${linear_5} + 5000) / 10000")
    set("${output}" "${result}" PARENT_SCOPE)
endfunction()

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../data/tokens.json" tokens)
set(checked 0)
foreach(mode IN ITEMS light dark)
    foreach(pair IN ITEMS
        br_text/br_bg br_text/br_surface br_muted/br_bg br_muted/br_surface
        br_on_accent/br_accent br_on_selected/br_selected br_success/br_success_bg
        br_warning/br_warning_bg br_error/br_error_bg br_info/br_info_bg br_accent/br_surface)
        string(REPLACE "/" ";" names "${pair}")
        list(GET names 0 foreground)
        list(GET names 1 background)
        string(JSON fg GET "${tokens}" color "${foreground}" "${mode}")
        string(JSON bg GET "${tokens}" color "${background}" "${mode}")
        luminance("${fg}" high)
        luminance("${bg}" low)
        if(high LESS low)
            set(swap "${high}")
            set(high "${low}")
            set(low "${swap}")
        endif()
        math(EXPR actual "(${high} + 50000) * 1000")
        math(EXPR minimum "4500 * (${low} + 50000)")
        math(EXPR ratio "${actual} / (${low} + 50000)")
        math(EXPR whole "${ratio} / 1000")
        math(EXPR fraction "${ratio} % 1000 + 1000")
        string(SUBSTRING "${fraction}" 1 3 fraction)
        if(actual LESS minimum)
            message(FATAL_ERROR "${mode} ${pair}: ${whole}.${fraction}:1 is below WCAG AA 4.5:1")
        endif()
        message(STATUS "${mode} ${pair}: ${whole}.${fraction}:1 PASS")
        math(EXPR checked "${checked} + 1")
    endforeach()
endforeach()
message(STATUS "Theme.Contrast: ${checked} text/background pairs pass WCAG AA")
