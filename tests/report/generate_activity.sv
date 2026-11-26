module generate_leaf(input wire i, output wire o);
    wire connected;
    assign connected = i;
    assign o = connected;
endmodule

module generate_activity(input wire i, output wire o);
    if (1) begin : enabled_branch
        wire live;
        generate_leaf active_leaf(i, live);
        assign o = live;
    end else begin : disabled_branch
        wire ghost;
        if (1) begin : nested
            wire nested_ghost;
            assign nested_ghost = i;
        end
        generate_leaf inactive_leaf(i, ghost);
    end
endmodule
